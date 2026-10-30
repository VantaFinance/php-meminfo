--TEST--
fatal-time S3 capture follows independently verified native OOM lifecycle
--SKIPIF--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $reason = meminfo_test_s3_skip_reason();
    if ($reason !== null) die('skip ' . $reason);
    if (getenv('USE_ZEND_ALLOC') === '0') die('skip memory_limit requires Zend MM');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';

    meminfo_test_s3_ensure_bucket();
    $native = meminfo_test_native_oom_behavior();

    $dir = meminfo_test_make_dump_dir();
    $child = meminfo_test_spawn('
        class LimitMarker {}
        register_shutdown_function(function () { fwrite(STDERR, "MEMINFO_NATIVE_SHUTDOWN\n"); });
        $marker = new LimitMarker();
        $grow = array();
        while (true) {
            $grow[] = str_repeat("x", 1024);
        }
    ', array_merge(meminfo_test_s3_ini(meminfo_test_s3_prefix('on_limit')), array(
        'meminfo.dump_dir'      => $dir,
        'meminfo.dump_on_limit' => '1',
        'memory_limit'          => '16M',
    )));
    $status = meminfo_test_finish_child($child);

    $output = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
    echo 'ended with the memory error: ', strpos($output, 'Allowed memory size') !== false ? 'yes' : 'no', "\n";
    echo 'native lifecycle preserved: ', $status === $native['status']
        && (strpos($output, 'MEMINFO_NATIVE_SHUTDOWN') !== false) === $native['shutdown'] ? 'yes' : 'no', "\n";

    if ($native['immediate_exit']) {
        $log = is_file($child['error_log']) ? file_get_contents($child['error_log']) : '';
        echo "fatal callback unavailable: native immediate exit\n";
        echo 'dump and upload artifacts: ', count(glob($dir . '/*')), "\n";
        echo 'shutdown drain absent: ', strpos($log, 'meminfo: waiting up to') === false ? 'yes' : 'no', "\n";
        meminfo_test_cleanup($child);
        meminfo_test_remove_dir($dir);
        return;
    }

    $dumps = glob($dir . '/meminfo-*.json');
    echo 'dumps: ', count($dumps), "\n";

    // Already there once the process is gone: it waited at exit.
    $url = count($dumps) === 1 && is_file($dumps[0] . '.uploaded') ? trim(file_get_contents($dumps[0] . '.uploaded')) : null;
    echo 'uploaded before the process exited: ', $url !== null ? 'yes' : 'no', "\n";

    $object = $url !== null ? meminfo_test_s3_get(meminfo_test_s3_key_of($url)) : array('status' => 0, 'headers' => array(), 'body' => '');
    echo 'GET: ', $object['status'], "\n";
    echo 'trigger: ', isset($object['headers']['x-amz-meta-meminfo-trigger']) ? $object['headers']['x-amz-meta-meminfo-trigger'] : '-', "\n";
    $decoded = json_decode($object['body'], true);
    echo 'header trigger: ', isset($decoded['header']['trigger']) ? $decoded['header']['trigger'] : '-', "\n";

    $log = file_get_contents($child['error_log']);
    echo 'log: dump written, upload in the background: ', strpos($log, 'its upload goes on in the background') !== false ? 'yes' : 'no', "\n";
    echo 'log: waited at exit: ', strpos($log, 'meminfo: waiting up to') !== false ? 'yes' : 'no', "\n";
    echo 'log: uploaded: ', strpos($log, 'meminfo: uploaded ') !== false ? 'yes' : 'no', "\n";

    meminfo_test_cleanup($child);
    array_map('unlink', glob($dir . '/*'));
    meminfo_test_remove_dir($dir);
?>
--EXPECTREGEX--
ended with the memory error: yes
native lifecycle preserved: yes
(?:fatal callback unavailable: native immediate exit
dump and upload artifacts: 0
shutdown drain absent: yes|dumps: 1
uploaded before the process exited: yes
GET: 200
trigger: memory_limit
header trigger: memory_limit
log: dump written, upload in the background: yes
log: waited at exit: yes
log: uploaded: yes)
