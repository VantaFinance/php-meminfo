--TEST--
invalid or overflowing thresholds are diagnosed once without activating capture
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80000 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') die('skip threshold target needs PHP 8+, NTS, POSIX');
    if (!function_exists('proc_open')) die('skip proc_open required');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';
    foreach (array('-1', 'junk', '1Mgarbage', '101%', '999999999999999999999G', '18446744073709551615G') as $threshold) {
        $dir = meminfo_test_make_dump_dir();
        $child = meminfo_test_spawn('
            $a = array();
            for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
        ', array('meminfo.dump_dir' => $dir, 'meminfo.dump_threshold' => $threshold, 'memory_limit' => '32M'));
        $status = proc_close($child['process']);
        $log = is_file($child['error_log']) ? file_get_contents($child['error_log']) : '';
        $log .= file_get_contents($child['stderr']);
        echo $threshold, ': ', $status === 0 && count(glob($dir . '/*.json')) === 0
            && substr_count($log, 'invalid meminfo.dump_threshold') === 1 ? 'rejected' : 'FAIL', "\n";
        meminfo_test_cleanup($child);
        meminfo_test_remove_dir($dir);
    }
?>
--EXPECT--
-1: rejected
junk: rejected
1Mgarbage: rejected
101%: rejected
999999999999999999999G: rejected
18446744073709551615G: rejected
