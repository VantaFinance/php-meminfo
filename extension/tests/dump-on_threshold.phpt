--TEST--
meminfo.dump_threshold writes one dump when memory usage passes it (8.0+)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (PHP_VERSION_ID < 80000) die('skip the threshold is checked from an observer, PHP 8.0+');
    if (PHP_ZTS) die('skip signal-path dumps are not available on ZTS builds');
    if (DIRECTORY_SEPARATOR === '\\') die('skip POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    $dumpDir = meminfo_test_make_dump_dir();

    // Grows well past the threshold through function calls (where it is checked),
    // then stops: one dump, not one per call.
    $targetCode = <<<'CODE'
class ThresholdMarker {}
// A user function: 8.0 and 8.1 observe only calls to those.
function chunk() {
    return str_repeat('x', 1024);
}
$marker = new ThresholdMarker();
$grow = array();
for ($i = 0; $i < 30000; $i++) {
    $grow[] = chunk();
}
echo "done\n";
CODE;

    $child = meminfo_test_spawn($targetCode, array(
        'meminfo.dump_dir' => $dumpDir,
        'meminfo.dump_threshold' => '8M',
        'memory_limit' => '128M',
    ));
    proc_close($child['process']);

    echo "target finished: ", trim(file_get_contents($child['stdout'])), "\n";
    $dumps = glob($dumpDir . '/meminfo-*.json');
    echo "dumps: ", count($dumps), "\n";
    if (count($dumps) === 1) {
        $items = meminfo_test_items($dumps[0]);
        echo "marker present: ", meminfo_test_has_class($items, 'ThresholdMarker') ? 'yes' : 'no', "\n";
        echo "trigger: ", json_decode(file_get_contents($dumps[0]), true)['header']['trigger'], "\n";
        unlink($dumps[0]);
    }
    $log = is_file($child['error_log']) ? file_get_contents($child['error_log']) : '';
    echo "logged: ", strpos($log, 'passed meminfo.dump_threshold') !== false ? 'yes' : 'no', "\n";

    meminfo_test_remove_dir($dumpDir);
?>
--EXPECT--
target finished: done
dumps: 1
marker present: yes
trigger: threshold
logged: yes
