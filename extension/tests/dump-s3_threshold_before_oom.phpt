--TEST--
an early threshold upload confirmed before OOM survives native fatal termination
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
    meminfo_test_s3_ensure_bucket();
    $native = meminfo_test_native_oom_behavior();
    $dir = meminfo_test_make_dump_dir();
    $child = meminfo_test_spawn('
        register_shutdown_function(function () { fwrite(STDERR, "MEMINFO_NATIVE_SHUTDOWN\n"); });
        class EarlyUploadMarker {}
        $marker = new EarlyUploadMarker();
        $a = array();
        for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
        $deadline = microtime(true) + 10;
        do {
            $uploaded = glob(ini_get("meminfo.dump_dir") . "/*.json.uploaded");
            if (count($uploaded) === 1) { break; }
            usleep(10000);
        } while (microtime(true) < $deadline);
        if (count($uploaded) !== 1) { fwrite(STDERR, "upload confirmation timed out\n"); exit(92); }
        echo "upload confirmed before fatal\n";
        while (true) { $a[] = str_repeat("x", 1024); }
    ', array_merge(meminfo_test_s3_ini(meminfo_test_s3_prefix('threshold_oom')), array(
        'meminfo.dump_dir' => $dir, 'meminfo.dump_threshold' => '4M', 'memory_limit' => '16M')));
    $status = meminfo_test_finish_child($child, 25);
    $output = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
    echo 'confirmed before fatal: ', strpos($output, 'upload confirmed before fatal') !== false ? 'yes' : 'no', "\n";
    echo 'native lifecycle preserved: ', $status === $native['status'] && strpos($output, 'Allowed memory size') !== false
        && (strpos($output, 'MEMINFO_NATIVE_SHUTDOWN') !== false) === $native['shutdown'] ? 'yes' : 'no', "\n";
    $dumps = glob($dir . '/*.json');
    echo 'local dump retained: ', count($dumps) === 1 ? 'yes' : 'no', "\n";
    $record = count($dumps) === 1 ? $dumps[0] . '.uploaded' : '';
    $object = is_file($record) ? meminfo_test_s3_get(meminfo_test_s3_key_of(trim(file_get_contents($record))))
        : array('status' => 0, 'body' => '', 'headers' => array());
    $data = json_decode($object['body'], true);
    echo 'GET: ', $object['status'], "\n";
    echo 'remote marker: ', isset($data['items']) && meminfo_test_has_class($data['items'], 'EarlyUploadMarker') ? 'yes' : 'no', "\n";
    echo 'header trigger: ', isset($data['header']['trigger']) ? $data['header']['trigger'] : '-', "\n";
    echo 'metadata trigger: ', isset($object['headers']['x-amz-meta-meminfo-trigger']) ? $object['headers']['x-amz-meta-meminfo-trigger'] : '-', "\n";
    meminfo_test_cleanup($child);
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
confirmed before fatal: yes
native lifecycle preserved: yes
local dump retained: yes
GET: 200
remote marker: yes
header trigger: threshold
metadata trigger: threshold
