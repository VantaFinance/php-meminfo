--TEST--
Check that the "dir" of meminfo_dump_process() decides where the target writes the dump
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
    if (!function_exists('meminfo_dump_process')) die('skip meminfo_dump_process() is not available');
    if (PHP_VERSION_ID < 70100) die('skip the target side needs PHP 7.1 or later');
    if (PHP_ZTS) die('skip the target side is not available on ZTS builds');
    if (!is_file(dirname(dirname(dirname(__FILE__))) . '/cli/bin/dump-meminfo')) die('skip cli/bin/dump-meminfo is not part of this source tree');
    if (!is_dir('/tmp') || !is_writable('/tmp')) die('skip /tmp is required for dump requests');
?>
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    // The target is configured to write somewhere else entirely. Before the
    // directory was handed to it, the trigger waited out its whole timeout
    // there, and every resend made the target write another full dump.

    $configuredDir = meminfo_test_make_dump_dir();
    $requestedDir = meminfo_test_make_dump_dir();

    $targetCode = <<<'CODE'
class DirTargetMarker
{
    public $tag = 'dir-target-marker';
}

$markerObject = new DirTargetMarker();

$deadline = microtime(true) + 30;
$total = 0;
while (microtime(true) < $deadline) {
    for ($i = 0; $i < 100000; $i++) {
        $total += $i;
    }
}
CODE;

    $child = meminfo_test_spawn($targetCode, array(
        'meminfo.dump_dir' => $configuredDir,
    ));
    $pid = meminfo_test_child_pid($child);

    echo "target started: ", $pid > 0 ? 'yes' : 'no', "\n";

    if ($pid > 0) {
        $requests = '/tmp/meminfo-request-' . $pid;

        // --- through the command, as the bug was reported --------------------

        $command = escapeshellarg(meminfo_test_php_binary())
            . ' -n -d ' . escapeshellarg('extension=' . meminfo_test_extension_path())
            . ' ' . escapeshellarg(dirname(dirname(dirname(__FILE__))) . '/cli/bin/dump-meminfo')
            . ' ' . (int) $pid
            . ' --dir=' . escapeshellarg($requestedDir)
            . ' --timeout=60';

        $startedAt = microtime(true);
        $output = array();
        $exitCode = 0;
        exec($command . ' 2>/dev/null', $output, $exitCode);
        $elapsed = microtime(true) - $startedAt;

        $path = count($output) === 1 ? $output[0] : null;

        echo "dump-meminfo exit code: ", $exitCode, "\n";
        echo "returned well before the timeout: ", $elapsed < 15 ? 'yes' : 'no', "\n";
        echo "dump is in the requested dir: ",
            ($path !== null && dirname($path) === realpath($requestedDir) && is_file($path)) ? 'yes' : 'no', "\n";
        echo "dump has the marker: ",
            ($path !== null && meminfo_test_has_class(meminfo_test_items($path), 'DirTargetMarker')) ? 'yes' : 'no', "\n";

        // Longer than the resend interval, so a resend served late would show.
        usleep(1500000);

        echo "exactly one dump in the requested dir: ",
            count(glob($requestedDir . '/meminfo-*.json')) === 1 ? 'yes' : 'no', "\n";
        echo "nothing in the configured dir: ", count(glob($configuredDir . '/*')) === 0 ? 'yes' : 'no', "\n";
        // Named after the pid alone, or after the pid and the id the signal carries.
        $left = array_merge(glob($requests), glob($requests . '-*'), glob($requests . '.error'));
        echo "request removed: ", empty($left) ? 'yes' : 'no', "\n";

        foreach (glob($requestedDir . '/*') as $file) {
            unlink($file);
        }

        // --- through the function, with a relative dir -----------------------

        $cwd = getcwd();
        chdir(dirname($requestedDir));
        $result = meminfo_test_try_dump_process($pid, array('dir' => basename($requestedDir), 'timeout' => 20));
        chdir($cwd);

        echo "relative dir resolved: ",
            ($result['path'] !== null && dirname($result['path']) === realpath($requestedDir)) ? 'yes' : 'no', "\n";

        if ($result['path'] !== null) {
            unlink($result['path']);
        }

        echo "missing dir refused: ",
            meminfo_test_try_dump_process($pid, array('dir' => $requestedDir . '/missing'))['exception']
                instanceof InvalidArgumentException ? 'yes' : 'no', "\n";

        // --- requests the target must not honour -----------------------------

        // A symlink at the request name, as anyone could plant in /tmp: a
        // plain signal, which carries no id, must not follow it.
        symlink($requestedDir, $requests);

        meminfo_test_signal_pid($pid, 'URG');
        $plain = meminfo_test_wait_for_dump($configuredDir, 'meminfo-*.json', 10.0);

        echo "symlinked request ignored: ",
            ($plain !== null && count(glob($requestedDir . '/*')) === 0) ? 'yes' : 'no', "\n";

        unlink($requests);
        foreach (glob($configuredDir . '/*') as $file) {
            unlink($file);
        }

        // A request that does not name an absolute directory is answered with
        // the reason instead of a dump anywhere.
        file_put_contents($requests, "relative/dir\n");
        meminfo_test_signal_pid($pid, 'URG');

        $deadline = microtime(true) + 10;
        while (!file_exists($requests . '.error') && microtime(true) < $deadline) {
            meminfo_test_spin();
        }

        $reason = file_exists($requests . '.error') ? file_get_contents($requests . '.error') : '';
        echo "invalid request answered: ", strpos($reason, 'absolute directory') !== false ? 'yes' : 'no', "\n";
        echo "no dump for an invalid request: ", count(glob($configuredDir . '/*')) === 0 ? 'yes' : 'no', "\n";

        @unlink($requests . '.error');
        @unlink($requests);

        echo "target still running: ", meminfo_test_process_exists($pid) ? 'yes' : 'no', "\n";
        echo "target wrote nothing to stdout: ", filesize($child['stdout']) === 0 ? 'yes' : 'no', "\n";
    }

    meminfo_test_cleanup($child, $pid);
    meminfo_test_remove_dir($configuredDir);
    meminfo_test_remove_dir($requestedDir);
?>
--EXPECT--
target started: yes
dump-meminfo exit code: 0
returned well before the timeout: yes
dump is in the requested dir: yes
dump has the marker: yes
exactly one dump in the requested dir: yes
nothing in the configured dir: yes
request removed: yes
relative dir resolved: yes
missing dir refused: yes
symlinked request ignored: yes
invalid request answered: yes
no dump for an invalid request: yes
target still running: yes
target wrote nothing to stdout: yes
