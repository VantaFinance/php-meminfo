--TEST--
A request never waits for an upload; only the exiting process does, for meminfo.dump_upload_timeout at most
--SKIPIF--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $reason = meminfo_test_s3_skip_reason();
    if ($reason !== null) die('skip ' . $reason);
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';

    // A server that accepts connections and never answers: the upload hangs
    // for as long as anybody is willing to wait.
    $server = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
    $address = stream_socket_get_name($server, false);

    $dir = meminfo_test_make_dump_dir();
    $started = microtime(true);
    $child = meminfo_test_spawn('
        exec("kill -URG " . getmypid());
        $afterDump = microtime(true);
        for ($i = 0; $i < 10; $i++) { usleep(1000); }
        echo "request went on for ", round(microtime(true) - $afterDump, 1) < 1 ? "under 1s" : "too long", "\n";
    ', array_merge(meminfo_test_s3_ini(meminfo_test_s3_prefix('no_wait')), array(
        'meminfo.dump_dir'            => $dir,
        'meminfo.s3_endpoint'         => 'http://' . $address,
        'meminfo.dump_upload_timeout' => '2',
    )));
    proc_close($child['process']);
    $elapsed = microtime(true) - $started;

    echo trim(file_get_contents($child['stdout'])), "\n";
    echo 'process exit bounded by the timeout: ', $elapsed >= 2 && $elapsed < 15 ? 'yes' : 'no (' . round($elapsed, 1) . 's)', "\n";

    $dumps = glob($dir . '/meminfo-*.json');
    echo 'dump kept: ', count($dumps) === 1 ? 'yes' : 'no', "\n";
    echo 'cancelled upload recorded: ', count($dumps) === 1 && is_file($dumps[0] . '.upload-error') ? 'yes' : 'no', "\n";

    $log = file_get_contents($child['error_log']);
    echo 'log: waiting up to 2s: ', strpos($log, 'meminfo: waiting up to 2s for 1 dump upload(s)') !== false ? 'yes' : 'no', "\n";
    echo 'log: gave up: ', strpos($log, 'meminfo: gave up on 1 dump upload(s) after 2s') !== false ? 'yes' : 'no', "\n";

    fclose($server);
    meminfo_test_cleanup($child);
    array_map('unlink', glob($dir . '/*'));
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
request went on for under 1s
process exit bounded by the timeout: yes
dump kept: yes
cancelled upload recorded: yes
log: waiting up to 2s: yes
log: gave up: yes
