--TEST--
A dump taken on a signal is uploaded to S3 in the background: same bytes, metadata, ".uploaded" record
--SKIPIF--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $reason = meminfo_test_s3_skip_reason();
    if ($reason !== null) die('skip ' . $reason);
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';

    meminfo_test_s3_ensure_bucket();

    $dir = meminfo_test_make_dump_dir();
    $prefix = meminfo_test_s3_prefix('signal');
    $ini = array_merge(meminfo_test_s3_ini($prefix), array(
        'meminfo.dump_dir' => $dir,
    ));

    // The child dumps itself, then keeps working for a while: the upload must
    // not hold it up, and it must be done well before the child exits.
    $child = meminfo_test_spawn('
        $keep = array_fill(0, 1000, str_repeat("x", 100));
        $start = microtime(true);
        exec("kill -URG " . getmypid());
        for ($i = 0; $i < 1000; $i++) { $keep[] = $i; }
        echo "dumped in ", (microtime(true) - $start) < 2 ? "under 2s" : "too long", "\n";
        usleep(3000000);
    ', $ini);

    $error = null;
    $dump = meminfo_test_wait_for_dump($dir, 'meminfo-*.json', 10.0);
    $url = $dump !== null ? meminfo_test_s3_wait_uploaded($dump, 20, $error) : null;

    proc_close($child['process']);

    echo 'dump written: ', $dump !== null ? 'yes' : 'no', "\n";
    echo 'uploaded: ', $url !== null ? 'yes' : 'no (' . trim((string) $error) . ')', "\n";
    echo 'url: ', preg_replace('#/[^/]+/meminfo-[^/]+\.json$#', '/<host>/<dump>', (string) $url) === 's3://' . meminfo_test_s3_bucket() . '/' . $prefix . '/<host>/<dump>' ? 'bucket/prefix/host/file' : $url, "\n";

    $object = $url !== null ? meminfo_test_s3_get(meminfo_test_s3_key_of($url)) : array('status' => 0, 'headers' => array(), 'body' => '');
    echo 'GET: ', $object['status'], "\n";
    echo 'same bytes: ', $dump !== null && $object['body'] === file_get_contents($dump) ? 'yes' : 'no', "\n";
    echo 'content type: ', isset($object['headers']['content-type']) ? $object['headers']['content-type'] : '-', "\n";
    echo 'trigger: ', isset($object['headers']['x-amz-meta-meminfo-trigger']) ? $object['headers']['x-amz-meta-meminfo-trigger'] : '-', "\n";
    echo 'pid matches: ', isset($object['headers']['x-amz-meta-meminfo-pid']) && $object['headers']['x-amz-meta-meminfo-pid'] === trim(file_get_contents($child['pid_file'])) ? 'yes' : 'no', "\n";
    echo 'local copy kept: ', $dump !== null && file_exists($dump) ? 'yes' : 'no', "\n";
    echo trim(file_get_contents($child['stdout'])), "\n";

    $log = file_get_contents($child['error_log']);
    echo 'log: uploading: ', strpos($log, 'meminfo: uploading ') !== false ? 'yes' : 'no', "\n";
    echo 'log: uploaded: ', preg_match('/meminfo: uploaded \S+ to s3:\/\/\S+ in [0-9.]+s/', $log) ? 'yes' : 'no', "\n";
    echo 'log: no waiting at exit: ', strpos($log, 'waiting up to') === false ? 'yes' : 'no', "\n";

    meminfo_test_cleanup($child);
    array_map('unlink', glob($dir . '/*'));
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
dump written: yes
uploaded: yes
url: bucket/prefix/host/file
GET: 200
same bytes: yes
content type: application/json
trigger: signal
pid matches: yes
local copy kept: yes
dumped in under 2s
log: uploading: yes
log: uploaded: yes
log: no waiting at exit: yes
