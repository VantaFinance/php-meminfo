--TEST--
internal threshold capture runs in a traced loop with JIT still enabled
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80100 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') die('skip JIT threshold target needs PHP 8.1+, NTS, POSIX');
    if (!function_exists('proc_open')) die('skip proc_open required');
    if (!is_file(ini_get('extension_dir') . '/opcache.so')) die('skip shared OPcache required');
    require __DIR__ . '/fixtures/signal_helpers.php';
    $control = meminfo_test_spawn('
        $jit = opcache_get_status(false);
        echo !empty($jit["jit"]["on"]) ? "JIT_AVAILABLE" : "JIT_UNAVAILABLE";
    ', meminfo_test_jit_options(), false);
    $status = meminfo_test_finish_child($control);
    $available = $status === 0 && trim(file_get_contents($control['stdout'])) === 'JIT_AVAILABLE';
    meminfo_test_cleanup($control);
    if (!$available) die('skip native PHP cannot enable JIT on this build');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';
    $dir = meminfo_test_make_dump_dir();
    $child = meminfo_test_spawn('
        $jit = opcache_get_status(false);
        $before = $jit["jit"]["buffer_free"];
        $a = array();
        for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
        $jit = opcache_get_status(false);
        echo "JIT active: ", !empty($jit["jit"]["on"]) ? "yes" : "no", "\n";
        echo "traced: ", $jit["jit"]["buffer_free"] < $before ? "yes" : "no", "\n";
    ', array_merge(meminfo_test_jit_options(), array('meminfo.dump_dir' => $dir,
        'meminfo.dump_threshold' => '4M', 'memory_limit' => '32M')));
    echo 'exit: ', proc_close($child['process']), "\n", file_get_contents($child['stdout']);
    $dumps = glob($dir . '/*.json');
    echo 'dumps: ', count($dumps), "\n";
    if (count($dumps) === 1) {
        $data = json_decode(file_get_contents($dumps[0]), true);
        echo 'trigger: ', $data['header']['trigger'], "\n";
    }
    meminfo_test_cleanup($child);
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
exit: 0
JIT active: yes
traced: yes
dumps: 1
trigger: threshold
