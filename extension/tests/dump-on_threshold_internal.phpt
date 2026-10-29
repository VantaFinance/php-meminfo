--TEST--
meminfo.dump_threshold observes internal-only allocation loops before OOM
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80000 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') die('skip threshold target needs PHP 8+, NTS, POSIX');
    if (!function_exists('proc_open')) die('skip proc_open required');
    if (getenv('USE_ZEND_ALLOC') === '0') die('skip memory_limit requires Zend MM');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';
    $dir = meminfo_test_make_dump_dir();
    $child = meminfo_test_spawn('
        class InternalThresholdMarker {}
        $marker = new InternalThresholdMarker();
        $grow = array();
        while (true) { $grow[] = str_repeat("x", 1024); }
    ', array('meminfo.dump_dir' => $dir, 'meminfo.dump_threshold' => '8M', 'memory_limit' => '16M'));
    $status = meminfo_test_finish_child($child);
    $output = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
    echo 'memory error: ', strpos($output, 'Allowed memory size') !== false ? 'yes' : 'no', "\n";
    echo 'failed request: ', $status !== 0 ? 'yes' : 'no', "\n";
    $dumps = glob($dir . '/meminfo-*.json');
    echo 'one early dump: ', count($dumps), "\n";
    if (count($dumps) === 1) {
        $data = json_decode(file_get_contents($dumps[0]), true);
        echo 'trigger: ', $data['header']['trigger'], "\n";
        echo 'marker: ', meminfo_test_has_class($data['items'], 'InternalThresholdMarker') ? 'yes' : 'no', "\n";
        echo 'growing frame: ', meminfo_test_frame_of($data['items'], 'grow'), "\n";
    }
    meminfo_test_cleanup($child);
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
memory error: yes
failed request: yes
one early dump: 1
trigger: threshold
marker: yes
growing frame: <GLOBAL>
