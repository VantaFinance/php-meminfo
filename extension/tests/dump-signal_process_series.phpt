--TEST--
Check that dump-meminfo --count takes a series of dumps of a running process
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (!file_exists('/proc/self/status') && PHP_OS !== 'Darwin') die('skip /proc or macOS is required to find the target');
    // The target is spawned from this same PHP, so it has to be a build that
    // can serve a dump. Refusing a target that cannot is covered by
    // dump-signal_not_handled.phpt instead.
    if (PHP_VERSION_ID < 70100) die('skip the target side needs PHP 7.1 or later');
    if (PHP_ZTS) die('skip the target side is not available on ZTS builds');
    if (!is_file(dirname(dirname(dirname(__FILE__))) . '/cli/bin/dump-meminfo')) die('skip cli/bin/dump-meminfo is not part of this source tree');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    $dumpDir = meminfo_test_make_dump_dir();

    $targetCode = <<<'CODE'
class SeriesTargetMarker
{
}

$kept = array();
$deadline = microtime(true) + 30;
while (microtime(true) < $deadline) {
    $kept[] = new SeriesTargetMarker();
    for ($i = 0; $i < 100000; $i++) {
    }
}
CODE;

    $child = meminfo_test_spawn($targetCode, array(
        'meminfo.dump_dir' => $dumpDir,
    ));
    $pid = meminfo_test_child_pid($child);

    echo "target started: ", $pid > 0 ? 'yes' : 'no', "\n";

    if ($pid > 0) {
        $command = escapeshellarg(meminfo_test_php_binary())
            . ' -n -d ' . escapeshellarg('extension=' . meminfo_test_extension_path())
            . ' ' . escapeshellarg(dirname(dirname(dirname(__FILE__))) . '/cli/bin/dump-meminfo')
            . ' ' . (int) $pid
            . ' --dir=' . escapeshellarg($dumpDir)
            . ' --timeout=20 --count=3 --interval=1';

        $commandStderr = $dumpDir . '/dump-meminfo.err';
        $output = array();
        $exitCode = 0;
        exec($command . ' 2>' . escapeshellarg($commandStderr), $output, $exitCode);
        $stderr = is_file($commandStderr) ? file_get_contents($commandStderr) : '';
        @unlink($commandStderr);

        echo "exit code: ", $exitCode, "\n";
        if ($exitCode !== 0) {
            echo "stderr: ", trim($stderr), "\n";
        }
        echo "paths printed: ", count($output), "\n";
        echo "distinct files: ", count(array_unique($output)) === count($output) ? 'yes' : 'no', "\n";
        echo "progress lines: ", preg_match_all('/^dump-meminfo: dump \d of 3 written in /m', $stderr), "\n";

        // The target keeps adding markers: every dump has more than the one before.
        $previous = -1;
        $growing = true;
        foreach ($output as $path) {
            $items = meminfo_test_items($path);
            $markers = 0;
            foreach ($items as $item) {
                if (isset($item['class']) && $item['class'] === 'SeriesTargetMarker') {
                    $markers++;
                }
            }
            $growing = $growing && $markers > $previous;
            $previous = $markers;
            unlink($path);
        }
        echo "markers grow from dump to dump: ", $growing ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
    meminfo_test_remove_dir($dumpDir);
?>
--EXPECT--
target started: yes
exit code: 0
paths printed: 3
distinct files: yes
progress lines: 3
markers grow from dump to dump: yes
