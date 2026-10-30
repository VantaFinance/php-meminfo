--TEST--
dump_on_limit preserves native OOM behavior and captures fatal errors on engines with error callbacks
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (PHP_VERSION_ID < 70100) die('skip signal-path dumps need PHP 7.1 or later');
    if (PHP_ZTS) die('skip signal-path dumps are not available on ZTS builds');
    if (DIRECTORY_SEPARATOR === '\\') die('skip POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
    if (getenv('USE_ZEND_ALLOC') === '0') die('skip memory_limit requires Zend MM');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    $dumpDir = meminfo_test_make_dump_dir();
    $native = meminfo_test_native_oom_behavior();

    $targetCode = <<<'CODE'
class LimitMarker {}
register_shutdown_function(function () { fwrite(STDERR, "MEMINFO_NATIVE_SHUTDOWN\n"); });
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
    $status = meminfo_test_finish_child($child);

    $stdout = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
    echo "target ended with the memory error: ", strpos($stdout, 'Allowed memory size') !== false ? 'yes' : 'no', "\n";
    echo 'native lifecycle preserved: ', $status === $native['status']
        && (strpos($stdout, 'MEMINFO_NATIVE_SHUTDOWN') !== false) === $native['shutdown'] ? 'yes' : 'no', "\n";

    $dumps = glob($dumpDir . '/meminfo-*.json');
    echo "dumps: ", count($dumps), "\n";
    if ($native['immediate_exit']) {
        echo "fatal callback unavailable: native immediate exit\n";
    }
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
    if (!$native['immediate_exit']) {
        echo "logged: ", strpos($log, 'memory_limit reached, dump written to') !== false ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child);
    meminfo_test_remove_dir($dumpDir);
?>
--EXPECTREGEX--
target ended with the memory error: yes
native lifecycle preserved: yes
(?:dumps: 0
fatal callback unavailable: native immediate exit|dumps: 1
valid json: yes
trigger: memory_limit
marker present: yes
frame of the growing array: <GLOBAL>
logged: yes)
