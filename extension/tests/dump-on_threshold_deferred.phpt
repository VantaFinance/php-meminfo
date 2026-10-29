--TEST--
internal-call threshold respects deferred capture and once-per-request behavior
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
        echo "before poll: ", count(glob(ini_get("meminfo.dump_dir") . "/meminfo-*.json")), "\n";
        $path = meminfo_dump_if_requested();
        $data = $path !== null ? json_decode(file_get_contents($path), true) : null;
        echo "trigger: ", $data === null ? "missing" : $data["header"]["trigger"], "\n";
        echo "second poll: ", meminfo_dump_if_requested() === null ? "empty" : "dump", "\n";
    ', array('meminfo.dump_dir' => $dir, 'meminfo.dump_threshold' => '4M',
        'meminfo.dump_defer' => '1', 'memory_limit' => '32M'));
    echo 'exit: ', proc_close($child['process']), "\n", file_get_contents($child['stdout']);
    meminfo_test_cleanup($child);
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
exit: 0
before poll: 0
trigger: threshold (deferred)
second poll: empty
