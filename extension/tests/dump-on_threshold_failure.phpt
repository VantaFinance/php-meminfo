--TEST--
failed threshold capture is attempted once and preserves native memory_limit enforcement
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80000 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') die('skip threshold target needs PHP 8+, NTS, POSIX');
    if (!function_exists('proc_open')) die('skip proc_open required');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';
    $dir = meminfo_test_make_dump_dir();
    $child = meminfo_test_spawn('
        $a = array();
        for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
        echo "continued: ", ini_get("memory_limit"), "\n";
        $large = str_repeat("x", 32 * 1024 * 1024);
        echo "unexpected allocation\n";
    ', array('meminfo.dump_dir' => $dir . '/missing', 'meminfo.dump_threshold' => '4M', 'memory_limit' => '16M'));
    echo 'failed request: ', proc_close($child['process']) !== 0 ? 'yes' : 'no', "\n";
    $output = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
    $log = file_get_contents($child['error_log']);
    echo 'continued after dump failure: ', strpos($output, 'continued: 16M') !== false ? 'yes' : 'no', "\n";
    echo 'memory limit enforced: ', strpos($output, 'Allowed memory size') !== false && strpos($output, 'unexpected allocation') === false ? 'yes' : 'no', "\n";
    echo 'capture attempts: ', substr_count($log, 'meminfo: cannot create '), "\n";
    echo 'final dumps: ', count(glob($dir . '/*.json')), "\n";
    meminfo_test_cleanup($child);
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
failed request: yes
continued after dump failure: yes
memory limit enforced: yes
capture attempts: 1
final dumps: 0
