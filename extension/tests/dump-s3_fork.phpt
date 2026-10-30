--TEST--
A process forked before any upload uploads its own dumps; one forked after its parent started uploading keeps them local
--SKIPIF--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';
    $reason = meminfo_test_s3_skip_reason();
    if ($reason !== null) die('skip ' . $reason);
    if (meminfo_test_pcntl_ini() === null) die('skip pcntl is not available');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/s3_helpers.php';

    meminfo_test_s3_ensure_bucket();

    $dir = meminfo_test_make_dump_dir();
    $code = '
        $dir = ' . var_export($dir, true) . ';

        // Dump this process on its own signal and wait for the outcome of
        // its upload: "uploaded", "failed" or "local" (none within 3s).
        function dump_self($dir) {
            $pid = getmypid();
            exec("kill -URG " . $pid);
            $deadline = microtime(true) + 15;
            $dump = null;
            while (microtime(true) < $deadline) {
                $found = glob($dir . "/meminfo-" . $pid . "-*.json");
                if ($found) { $dump = $found[0]; break; }
                usleep(20000);
            }
            if ($dump === null) { return "no dump"; }
            $deadline = microtime(true) + 15;
            $localUntil = microtime(true) + 3;
            while (microtime(true) < $deadline) {
                if (is_file($dump . ".uploaded")) { return "uploaded"; }
                if (is_file($dump . ".upload-error")) { return "failed"; }
                if (microtime(true) > $localUntil) { return "local"; }
                usleep(20000);
            }
            return "local";
        }

        // Forked before anything was uploaded here: uploads on its own.
        $early = pcntl_fork();
        if ($early === 0) {
            echo "early child: ", dump_self($dir), "\n";
            exit(0);
        }
        pcntl_waitpid($early, $status);

        echo "parent: ", dump_self($dir), "\n";

        // Forked after the parent started uploading: keeps its dumps local.
        $late = pcntl_fork();
        if ($late === 0) {
            echo "late child: ", dump_self($dir), "\n";
            exit(0);
        }
        pcntl_waitpid($late, $status);

        echo "parent again: ", dump_self($dir), "\n";
    ';

    $child = meminfo_test_spawn($code, array_merge(meminfo_test_s3_ini(meminfo_test_s3_prefix('fork')), array(
        'meminfo.dump_dir' => $dir,
    ), meminfo_test_pcntl_ini()));
    proc_close($child['process']);

    echo trim(file_get_contents($child['stdout'])), "\n";
    $log = file_get_contents($child['error_log']);
    echo 'log: uploads off in the late child, once: ', substr_count($log, 'dump uploads are off in process'), "\n";

    meminfo_test_cleanup($child);
    array_map('unlink', glob($dir . '/*'));
    meminfo_test_remove_dir($dir);
?>
--EXPECT--
early child: uploaded
parent: uploaded
late child: local
parent again: uploaded
log: uploads off in the late child, once: 1
