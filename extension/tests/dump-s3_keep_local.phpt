--TEST--
meminfo.dump_upload_keep_local=0 removes the dump once uploaded; its ".uploaded" record stands for it, also for the trigger
--SKIPIF--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $reason = meminfo_test_s3_skip_reason();
    if ($reason !== null) die('skip ' . $reason);
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (!file_exists('/proc/self/status') && PHP_OS !== 'Darwin') die('skip /proc or macOS is required to find the target');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';

    meminfo_test_s3_ensure_bucket();

    $dir = meminfo_test_make_dump_dir();
    $child = meminfo_test_spawn('
        class KeepLocalMarker {}
        $marker = new KeepLocalMarker();
        // Busy until told to stop, then exits normally: the log lines of the
        // upload are written at the next poll point, here the exit.
        $stop = ' . var_export($dir . '/stop', true) . ';
        $deadline = microtime(true) + 60;
        $total = 0;
        while (microtime(true) < $deadline && !file_exists($stop)) {
            for ($i = 0; $i < 100000; $i++) { $total += $i; }
        }
    ', array_merge(meminfo_test_s3_ini(meminfo_test_s3_prefix('keep_local')), array(
        'meminfo.dump_dir'                => $dir,
        'meminfo.dump_upload_keep_local'  => '0',
    )));
    $pid = meminfo_test_child_pid($child);

    // Works on both trigger paths: a named dump (sigqueue) and one found by
    // scanning for the pid (plain kill, e.g. a build without sigqueue).
    $url = meminfo_dump_process($pid, array('dir' => $dir, 'timeout' => 60, 'wait_upload' => true));
    echo 'location: ', strpos($url, 's3://') === 0 ? 's3://…' : $url, "\n";

    $records = glob($dir . '/meminfo-*.json.uploaded');
    echo 'records: ', count($records), "\n";
    echo 'local copy removed: ', count($records) === 1 && !file_exists(substr($records[0], 0, -strlen('.uploaded'))) ? 'yes' : 'no', "\n";
    echo 'record holds the location: ', count($records) === 1 && trim(file_get_contents($records[0])) === $url ? 'yes' : 'no', "\n";
    printf("record mode: %o\n", count($records) === 1 ? fileperms($records[0]) & 0777 : 0);

    $object = meminfo_test_s3_get(meminfo_test_s3_key_of($url));
    echo 'object is the dump: ', strpos($object['body'], 'KeepLocalMarker') !== false ? 'yes' : 'no', "\n";

    touch($dir . '/stop');
    proc_close($child['process']);
    $log = file_get_contents($child['error_log']);
    echo 'log: local copy removed: ', strpos($log, ', local copy removed') !== false ? 'yes' : 'no', "\n";

    meminfo_test_cleanup($child, $pid);
    array_map('unlink', glob($dir . '/*'));
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
location: s3://…
records: 1
local copy removed: yes
record holds the location: yes
record mode: 600
object is the dump: yes
log: local copy removed: yes
