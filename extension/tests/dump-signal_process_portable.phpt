--TEST--
Check that meminfo_dump_process() finds the dump of another process without /proc or sigqueue()
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (PHP_VERSION_ID < 70100) die('skip the target side needs PHP 7.1 or later');
    if (PHP_ZTS) die('skip the target side is not available on ZTS builds');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    // Runs everywhere, /proc or not. Built without sigqueue() (macOS, or
    // ac_cv_func_sigqueue=no), the target names the dump after its pid and the
    // time, and the trigger has to find it among other files of that shape.

    $dumpDir = meminfo_test_make_dump_dir();

    $targetCode = <<<'CODE'
class PortableTargetMarker
{
    public $tag = 'portable-target-marker';
}

$markerObject = new PortableTargetMarker();

$deadline = microtime(true) + 30;
$total = 0;
while (microtime(true) < $deadline) {
    for ($i = 0; $i < 100000; $i++) {
        $total += $i;
    }
}
CODE;

    $child = meminfo_test_spawn($targetCode, array(
        'meminfo.dump_dir' => $dumpDir,
    ));
    $pid = meminfo_test_child_pid($child);

    echo "target started: ", $pid > 0 ? 'yes' : 'no', "\n";

    if ($pid > 0) {
        // Same shape as a genuine pid-named dump, but timestamped before the
        // request (left over from an earlier one) and in the future (planted).
        $stale = $dumpDir . '/meminfo-' . $pid . '-1-0.json';
        $future = $dumpDir . '/meminfo-' . $pid . '-' . (time() + 3600) . '-0.json';
        file_put_contents($stale, '{}');
        file_put_contents($future, '{}');

        $result = meminfo_test_try_dump_process($pid, array('dir' => $dumpDir, 'timeout' => 20));
        $path = $result['path'];

        if ($result['exception'] !== null) {
            echo "exception: ", $result['exception']->getMessage(), "\n";
        }

        echo "returned a path: ", is_string($path) ? 'yes' : 'no', "\n";
        echo "stale file not returned: ", $path !== $stale ? 'yes' : 'no', "\n";
        echo "future file not returned: ", $path !== $future ? 'yes' : 'no', "\n";

        if (is_string($path)) {
            $items = meminfo_test_items($path);

            echo "valid json: ", empty($items) ? 'no' : 'yes', "\n";
            echo "target marker present: ",
                meminfo_test_has_class($items, 'PortableTargetMarker') ? 'yes' : 'no', "\n";
        }

        // Longer than the resend interval: a resend that was served after the
        // dump had been found would show up here as a second file.
        usleep(1500000);

        // Compared by name: the returned path is resolved (on macOS /var is
        // /private/var), the glob is not.
        $known = array(basename((string) $path), basename($stale), basename($future));
        $extra = array();
        foreach (glob($dumpDir . '/meminfo-*') as $file) {
            if (!in_array(basename($file), $known, true)) {
                $extra[] = basename($file);
            }
        }

        echo "no stray dump: ", empty($extra) ? 'yes' : 'no (' . implode(', ', $extra) . ')', "\n";
        echo "target still running: ", meminfo_test_process_exists($pid) ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
    meminfo_test_remove_dir($dumpDir);
?>
--EXPECT--
target started: yes
returned a path: yes
stale file not returned: yes
future file not returned: yes
valid json: yes
target marker present: yes
no stray dump: yes
target still running: yes
