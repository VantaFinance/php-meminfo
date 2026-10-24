<?php
/**
 * How fast meminfo_dump() writes a large graph.
 *
 * Builds a graph of about --items items (objects with declared and dynamic
 * properties, packed and hash arrays, strings shared by many holders, closures
 * with captured variables), then times --runs dumps to each target and prints
 * one JSON document on stdout: items written, median and per-run milliseconds,
 * items per second and peak RSS. Progress goes to stderr.
 *
 *   php -d extension=.../meminfo.so bench-dump.php [--items=2000000] [--runs=5] [--target=null|file|both]
 *
 * Before timing, the same builder is run at 1/100 scale and that dump is
 * decoded with json_decode(): the benchmark refuses to report numbers for a
 * dump that is not valid JSON or lost items. Decoding the full-size dump would
 * need several GB.
 *
 * Kept parseable by PHP 7.0, the oldest version the extension supports.
 */

$options = getopt('', array('items:', 'runs:', 'target:'));
$items   = isset($options['items']) ? (int) $options['items'] : 2000000;
$runs    = isset($options['runs']) ? (int) $options['runs'] : 5;
$target  = isset($options['target']) ? $options['target'] : 'both';

if (!extension_loaded('meminfo')) {
    fwrite(STDERR, "error: the meminfo extension is not loaded (php -d extension=.../meminfo.so)\n");
    exit(1);
}

if ($items < 1000 || $runs < 1 || !in_array($target, array('null', 'file', 'both'), true)) {
    fwrite(STDERR, "error: usage: bench-dump.php [--items=N>=1000] [--runs=N>=1] [--target=null|file|both]\n");
    exit(1);
}

#[AllowDynamicProperties]
class BenchNode
{
    public $id;
    public $name;
    public $tags;
    protected $parent;
    private $payload;

    public function __construct($id, $name, $parent, $payload)
    {
        $this->id      = $id;
        $this->name    = $name;
        $this->parent  = $parent;
        $this->payload = $payload;
        $this->tags    = array($id, $id + 1, $id + 2);
    }
}

/**
 * Build a graph of roughly $items dump items. One unit below is 11 to 14
 * items: a node object, its 5 declared and 1 dynamic properties, the 3 elements
 * of its tags array and the 4 entries of its payload hash array; every 10th
 * unit adds a closure. The fewer count is for extensions that dump the 64
 * shared strings once (identity_model 2) instead of once per holder.
 *
 * @return array the graph; keep it referenced while dumping
 */
function bench_build($items)
{
    $shared = array();
    for ($i = 0; $i < 64; $i++) {
        $shared[] = str_repeat(chr(65 + $i % 26), 100 + $i);
    }

    $units = (int) ($items / 14);
    $graph = array('nodes' => array(), 'index' => array(), 'callbacks' => array());
    $previous = null;

    for ($i = 0; $i < $units; $i++) {
        $node = new BenchNode($i, $shared[$i % 64], $previous, array(
            'key'   => 'node-' . $i,
            'value' => $i * 2,
            'label' => $shared[($i * 7) % 64],
            'ok'    => true,
        ));
        $node->dynamic = $i;
        $graph['nodes'][] = $node;
        $graph['index']['n' . $i] = $node;

        if ($i % 10 === 0) {
            $graph['callbacks'][] = function () use ($node, $i) {
                return $node->id + $i;
            };
        }

        $previous = $i % 100 === 0 ? null : $node;
    }

    return $graph;
}

/**
 * @return int peak resident size in bytes, or 0 where it cannot be read
 */
function bench_peak_rss()
{
    if (!function_exists('getrusage')) {
        return 0;
    }

    $usage = getrusage();

    // ru_maxrss is in KiB on Linux and in bytes on macOS.
    return PHP_OS === 'Darwin' ? (int) $usage['ru_maxrss'] : (int) $usage['ru_maxrss'] * 1024;
}

function bench_now()
{
    return function_exists('hrtime') ? hrtime(true) / 1e6 : microtime(true) * 1000;
}

/**
 * Dump to $path and return the elapsed milliseconds.
 */
function bench_dump_once($path)
{
    $stream = fopen($path, 'w');
    if ($stream === false) {
        fwrite(STDERR, "error: cannot open $path for writing\n");
        exit(1);
    }

    $start = bench_now();
    meminfo_dump($stream);
    $elapsed = bench_now() - $start;
    fclose($stream);

    return $elapsed;
}

/**
 * Count the items of a dump without loading it: every item has exactly one
 * "is_root" line.
 */
function bench_count_items($path)
{
    $stream = fopen($path, 'r');
    $count = 0;
    while (($line = fgets($stream)) !== false) {
        if (strpos($line, '"is_root"') !== false) {
            $count++;
        }
    }
    fclose($stream);

    return $count;
}

function bench_median(array $values)
{
    sort($values);
    $count = count($values);
    $middle = (int) ($count / 2);

    return $count % 2 === 1 ? $values[$middle] : ($values[$middle - 1] + $values[$middle]) / 2;
}

// Validate the output on a small graph first.
$checkFile = tempnam(sys_get_temp_dir(), 'meminfo-bench-');
$small = bench_build((int) ($items / 100));
bench_dump_once($checkFile);
$decoded = json_decode(file_get_contents($checkFile), true);
unlink($checkFile);
$expectedMin = (int) ($items / 100 / 14) * 10;

if (!is_array($decoded) || !isset($decoded['items'])) {
    fwrite(STDERR, "error: the validation dump is not valid JSON: " . json_last_error_msg() . "\n");
    exit(1);
}

if (count($decoded['items']) < $expectedMin) {
    fwrite(STDERR, sprintf("error: the validation dump has %d items, expected at least %d\n", count($decoded['items']), $expectedMin));
    exit(1);
}

fwrite(STDERR, sprintf("bench: validation dump OK, %d items\n", count($decoded['items'])));
unset($small, $decoded);

$graph = bench_build($items);

$file = tempnam(sys_get_temp_dir(), 'meminfo-bench-');
$targets = $target === 'both' ? array('null' => '/dev/null', 'file' => $file) : array($target => $target === 'null' ? '/dev/null' : $file);

// Items actually written, counted from one dump to the file.
bench_dump_once($file);
$written = bench_count_items($file);
fwrite(STDERR, sprintf("bench: %d items in the graph, %d MB dump\n", $written, filesize($file) >> 20));

$result = array(
    'php'      => PHP_VERSION,
    'os'       => PHP_OS,
    'items'    => $written,
    'runs'     => $runs,
    'targets'  => array(),
);

foreach ($targets as $name => $path) {
    $times = array();
    for ($run = 1; $run <= $runs; $run++) {
        $times[] = bench_dump_once($path);
        fwrite(STDERR, sprintf("bench: %s run %d/%d: %.1f ms\n", $name, $run, $runs, end($times)));
    }

    $median = bench_median($times);
    $result['targets'][$name] = array(
        'median_ms'    => round($median, 1),
        'runs_ms'      => array_map(function ($time) { return round($time, 1); }, $times),
        'items_per_s'  => (int) ($written / ($median / 1000)),
    );
}

unlink($file);
$result['peak_rss'] = bench_peak_rss();

echo json_encode($result, JSON_PRETTY_PRINT), "\n";
