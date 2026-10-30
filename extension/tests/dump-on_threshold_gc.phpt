--TEST--
threshold observation preserves native reclamation and automatic collection at memory_limit
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80000 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') die('skip threshold target needs PHP 8+, NTS, POSIX');
    if (!function_exists('proc_open')) die('skip proc_open required');
    if (getenv('USE_ZEND_ALLOC') === '0') die('skip native Zend MM required');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';
    foreach (array('' => 'baseline', '4M' => 'threshold') as $threshold => $name) {
        $dir = meminfo_test_make_dump_dir();
        $child = meminfo_test_spawn('
            $a = array();
            for ($i = 0; $i < 15000; $i++) { $a[] = str_repeat("x", 512); }
            unset($a);
            // This only succeeds if native limit checks can reclaim unused chunks.
            $large = str_repeat("y", 9 * 1024 * 1024);
            echo "large allocation: ", strlen($large), "\n";
            unset($large);
            $a = array();
            for ($i = 0; $i < 15000; $i++) { $a[] = str_repeat("x", 512); }
            unset($a);
            echo "reclaimed: ", gc_mem_caches() > 0 ? "yes" : "no", "\n";
        ', array('meminfo.dump_dir' => $dir, 'meminfo.dump_threshold' => $threshold, 'memory_limit' => '16M'));
        $status = proc_close($child['process']);
        echo $name, ' exit: ', $status, "\n", file_get_contents($child['stdout']);
        echo 'dumps: ', count(glob($dir . '/meminfo-*.json')), "\n";
        meminfo_test_cleanup($child);
        meminfo_test_remove_dir($dir);
    }
?>
--EXPECT--
baseline exit: 0
large allocation: 9437184
reclaimed: yes
dumps: 0
threshold exit: 0
large allocation: 9437184
reclaimed: yes
dumps: 1
