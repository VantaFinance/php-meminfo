--TEST--
an upload held without a server response leaves its early dump local at native OOM
--SKIPIF--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $reason = meminfo_test_s3_skip_reason();
    if ($reason !== null) die('skip ' . $reason);
    if (PHP_VERSION_ID < 80000) die('skip threshold needs PHP 8.0+');
    if (getenv('USE_ZEND_ALLOC') === '0') die('skip memory_limit requires Zend MM');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $native = meminfo_test_native_oom_behavior();
    $dir = meminfo_test_make_dump_dir();
    $server = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
    $address = stream_socket_get_name($server, false);
    $release = $dir . '/release';
    $child = meminfo_test_spawn('
        register_shutdown_function(function () { fwrite(STDERR, "MEMINFO_NATIVE_SHUTDOWN\n"); });
        $a = array();
        for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
        $deadline = microtime(true) + 10;
        while (!is_file(' . var_export($release, true) . ') && microtime(true) < $deadline) { usleep(10000); }
        if (!is_file(' . var_export($release, true) . ')) { exit(92); }
        while (true) { $a[] = str_repeat("x", 1024); }
    ', array_merge(meminfo_test_s3_ini(meminfo_test_s3_prefix('threshold_interrupted')), array(
        'meminfo.dump_dir' => $dir, 'meminfo.dump_threshold' => '4M', 'memory_limit' => '16M',
        'meminfo.s3_endpoint' => 'http://' . $address, 'meminfo.dump_upload_timeout' => '1',
        'meminfo.dump_upload_keep_local' => '0')));
    $connection = false;
    try {
        // No response is ever sent: upload completion is impossible, independent of timing.
        $connection = stream_socket_accept($server, 10);
        echo 'upload connection accepted: ', $connection !== false ? 'yes' : 'no', "\n";
        file_put_contents($release, 'fatal may proceed');
        $status = meminfo_test_finish_child($child, 20);
        $output = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
        echo 'native lifecycle preserved: ', $status === $native['status'] && strpos($output, 'Allowed memory size') !== false
            && (strpos($output, 'MEMINFO_NATIVE_SHUTDOWN') !== false) === $native['shutdown'] ? 'yes' : 'no', "\n";
        $dumps = glob($dir . '/*.json');
        $data = count($dumps) === 1 ? json_decode(file_get_contents($dumps[0]), true) : null;
        echo 'local finalized dump: ', isset($data['header']['trigger']) && $data['header']['trigger'] === 'threshold' ? 'yes' : 'no', "\n";
        echo 'no success sidecar: ', count(glob($dir . '/*.uploaded')) === 0 ? 'yes' : 'no', "\n";
    } finally {
        if (is_resource($connection)) { fclose($connection); }
        fclose($server);
        meminfo_test_cleanup($child);
        meminfo_test_remove_dir($dir);
    }
?>
--EXPECT--
upload connection accepted: yes
native lifecycle preserved: yes
local finalized dump: yes
no success sidecar: yes
