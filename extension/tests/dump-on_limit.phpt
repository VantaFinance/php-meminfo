--TEST--
meminfo.dump_on_limit writes a dump when memory_limit is reached, before the fatal error
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (PHP_VERSION_ID < 70100) die('skip signal-path dumps need PHP 7.1 or later');
    if (PHP_ZTS) die('skip signal-path dumps are not available on ZTS builds');
    if (DIRECTORY_SEPARATOR === '\\') die('skip POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    $dumpDir = meminfo_test_make_dump_dir();

    $targetCode = <<<'CODE'
class LimitMarker {}
$marker = new LimitMarker();
$grow = array();
while (true) {
    $grow[] = str_repeat('x', 1024);
}
CODE;

    $child = meminfo_test_spawn($targetCode, array(
        'meminfo.dump_dir' => $dumpDir,
        'meminfo.dump_on_limit' => '1',
        'memory_limit' => '16M',
    ));
    $status = proc_close($child['process']);

    $stdout = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
    echo "target ended with the memory error: ", strpos($stdout, 'Allowed memory size') !== false ? 'yes' : 'no', "\n";

    $dumps = glob($dumpDir . '/meminfo-*.json');
    echo "one dump: ", count($dumps), "\n";
    if (count($dumps) === 1) {
        $items = meminfo_test_items($dumps[0]);
        echo "valid json: ", empty($items) ? 'no' : 'yes', "\n";
        $header = json_decode(file_get_contents($dumps[0]), true)['header'];
        echo "trigger: ", $header['trigger'], "\n";
        echo "marker present: ", meminfo_test_has_class($items, 'LimitMarker') ? 'yes' : 'no', "\n";
        echo "frame of the growing array: ", meminfo_test_frame_of($items, 'grow'), "\n";
        unlink($dumps[0]);
    }
    $log = is_file($child['error_log']) ? file_get_contents($child['error_log']) : '';
    echo "logged: ", strpos($log, 'memory_limit reached, dump written to') !== false ? 'yes' : 'no', "\n";

    meminfo_test_remove_dir($dumpDir);
?>
--EXPECT--
target ended with the memory error: yes
one dump: 1
valid json: yes
trigger: memory_limit
marker present: yes
frame of the growing array: <GLOBAL>
logged: yes
