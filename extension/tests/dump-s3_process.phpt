--TEST--
meminfo_dump_process() and dump-meminfo return the s3:// location with wait_upload / --wait-upload, the local path without
--SKIPIF--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $reason = meminfo_test_s3_skip_reason();
    if ($reason !== null) die('skip ' . $reason);
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (!file_exists('/proc/self/status') && PHP_OS !== 'Darwin') die('skip /proc or macOS is required to find the target');
    if (!is_file(dirname(dirname(__DIR__)) . '/cli/bin/dump-meminfo')) die('skip cli/bin/dump-meminfo is not part of this source tree');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';

    meminfo_test_s3_ensure_bucket();

    $dir = meminfo_test_make_dump_dir();
    $prefix = meminfo_test_s3_prefix('process');
    $child = meminfo_test_spawn('
        $deadline = microtime(true) + 60;
        $total = 0;
        while (microtime(true) < $deadline) {
            for ($i = 0; $i < 100000; $i++) { $total += $i; }
        }
    ', array_merge(meminfo_test_s3_ini($prefix), array('meminfo.dump_dir' => $dir)));
    $pid = meminfo_test_child_pid($child);
    echo 'target started: ', $pid > 0 ? 'yes' : 'no', "\n";

    $local = meminfo_dump_process($pid, array('dir' => $dir, 'timeout' => 20));
    echo 'without wait_upload, a local path: ', is_string($local) && $local[0] === '/' && is_file($local) ? 'yes' : 'no', "\n";

    $url = meminfo_dump_process($pid, array('dir' => $dir, 'timeout' => 20, 'wait_upload' => true));
    echo 'with wait_upload, an s3:// location: ', strpos($url, 's3://' . meminfo_test_s3_bucket() . '/' . $prefix . '/') === 0 ? 'yes' : 'no (' . $url . ')', "\n";
    $object = meminfo_test_s3_get(meminfo_test_s3_key_of($url));
    echo 'object there: ', $object['status'], "\n";

    $command = escapeshellarg(meminfo_test_php_binary()) . ' -n -d ' . escapeshellarg('extension=' . meminfo_test_extension_path())
        . ' ' . escapeshellarg(dirname(dirname(__DIR__)) . '/cli/bin/dump-meminfo')
        . ' ' . (int) $pid . ' --dir=' . escapeshellarg($dir) . ' --timeout=20 --wait-upload 2>&1';
    $printed = trim((string) shell_exec($command));
    echo 'dump-meminfo --wait-upload prints the location: ', strpos($printed, 's3://' . meminfo_test_s3_bucket() . '/' . $prefix . '/') === 0 ? 'yes' : 'no (' . $printed . ')', "\n";

    meminfo_test_cleanup($child, $pid);
    array_map('unlink', glob($dir . '/*'));
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
target started: yes
without wait_upload, a local path: yes
with wait_upload, an s3:// location: yes
object there: 200
dump-meminfo --wait-upload prints the location: yes
