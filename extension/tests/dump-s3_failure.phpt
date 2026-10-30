--TEST--
Failed uploads leave the dump local, record why in ".upload-error" and the log, and never reach the output
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

    /**
     * Dump a child on its own signal with these settings and report.
     */
    function failing_upload($label, array $ini, array $env = array())
    {
        $saved = array();
        foreach ($env as $name => $value) {
            $saved[$name] = getenv($name);
            putenv($value === null ? $name : $name . '=' . $value);
        }

        $dir = meminfo_test_make_dump_dir();
        $child = meminfo_test_spawn('
            exec("kill -URG " . getmypid());
            for ($i = 0; $i < 100; $i++) { usleep(10000); }
            echo "child output\n";
        ', array_merge(meminfo_test_s3_ini(meminfo_test_s3_prefix('failure')), $ini, array(
            'meminfo.dump_dir'            => $dir,
            'meminfo.dump_upload_timeout' => '20',
        )));
        proc_close($child['process']);

        foreach ($saved as $name => $value) {
            putenv($value === false ? $name : $name . '=' . $value);
        }

        $dumps = glob($dir . '/meminfo-*.json');
        $error = count($dumps) === 1 && is_file($dumps[0] . '.upload-error') ? file_get_contents($dumps[0] . '.upload-error') : '';
        $log = file_get_contents($child['error_log']);

        echo $label, ":\n";
        echo '  dump kept: ', count($dumps) === 1 ? 'yes' : 'no', "\n";
        echo '  no .uploaded: ', count($dumps) === 1 && !is_file($dumps[0] . '.uploaded') ? 'yes' : 'no', "\n";
        echo '  error recorded: ', $error !== '' ? 'yes' : 'no', "\n";
        echo '  logged: ', strpos($log, 'meminfo: upload of ') !== false && strpos($log, 'failed') !== false ? 'yes' : 'no', "\n";
        echo '  output untouched: ', trim(file_get_contents($child['stdout'])) === 'child output' ? 'yes' : 'no', "\n";

        meminfo_test_cleanup($child);
        array_map('unlink', glob($dir . '/*'));
        meminfo_test_remove_dir($dir);

        return $error;
    }

    $error = failing_upload('no credentials', array(), array('AWS_ACCESS_KEY_ID' => null));
    echo '  says why: ', strpos($error, 'no credentials') !== false && strpos($error, 'clear_env') !== false ? 'yes' : 'no', "\n";

    $error = failing_upload('wrong secret', array(), array('AWS_SECRET_ACCESS_KEY' => 'not-the-secret'));
    echo '  HTTP 403: ', strpos($error, 'HTTP 403') !== false ? 'yes' : 'no (' . $error . ')', "\n";

    $error = failing_upload('missing bucket', array('meminfo.dump_upload' => 's3://no-such-bucket-' . mt_rand(1000, 9999)));
    echo '  HTTP 404: ', strpos($error, 'HTTP 404') !== false ? 'yes' : 'no (' . $error . ')', "\n";

    $error = failing_upload('nothing listening', array('meminfo.s3_endpoint' => 'http://127.0.0.1:9'));
    echo '  connection error: ', $error !== '' && strpos($error, 'HTTP') === false ? 'yes' : 'no (' . $error . ')', "\n";
?>
--EXPECT--
no credentials:
  dump kept: yes
  no .uploaded: yes
  error recorded: yes
  logged: yes
  output untouched: yes
  says why: yes
wrong secret:
  dump kept: yes
  no .uploaded: yes
  error recorded: yes
  logged: yes
  output untouched: yes
  HTTP 403: yes
missing bucket:
  dump kept: yes
  no .uploaded: yes
  error recorded: yes
  logged: yes
  output untouched: yes
  HTTP 404: yes
nothing listening:
  dump kept: yes
  no .uploaded: yes
  error recorded: yes
  logged: yes
  output untouched: yes
  connection error: yes
