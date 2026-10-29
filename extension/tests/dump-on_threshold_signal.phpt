--TEST--
an explicit signal keeps its trigger when coalesced with a deferred threshold
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80000 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') die('skip threshold target needs PHP 8+, NTS, POSIX');
    if (!function_exists('proc_open') || !function_exists('exec')) die('skip child process and kill required');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';
    $dir = meminfo_test_make_dump_dir();
    $signal = meminfo_test_default_signal();
    $child = meminfo_test_spawn('
        $a = array();
        for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
        exec("kill -' . $signal . ' " . getmypid());
        $path = meminfo_dump_if_requested();
        $data = $path !== null ? json_decode(file_get_contents($path), true) : null;
        echo "trigger: ", $data === null ? "missing" : $data["header"]["trigger"], "\n";
        echo "second poll: ", meminfo_dump_if_requested() === null ? "empty" : "dump", "\n";
    ', array('meminfo.dump_dir' => $dir, 'meminfo.dump_threshold' => '4M',
        'meminfo.dump_defer' => '1', 'memory_limit' => '32M'));
    echo 'exit: ', meminfo_test_finish_child($child), "\n", file_get_contents($child['stdout']);
    echo 'dumps: ', count(glob($dir . '/*.json')), "\n";
    meminfo_test_cleanup($child);
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
exit: 0
trigger: deferred
second poll: empty
dumps: 1
