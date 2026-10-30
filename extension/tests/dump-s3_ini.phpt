--TEST--
meminfo.dump_upload is validated at startup: a bad target or a build that cannot upload is reported once, dumps stay local
--SKIPIF--
<?php
    if (DIRECTORY_SEPARATOR === '\\') die('skip POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';

    function startup_output(array $ini)
    {
        $child = meminfo_test_spawn('echo "ran\n";', $ini);
        proc_close($child['process']);

        $output = file_get_contents($child['stdout']) . file_get_contents($child['stderr'])
            . (is_file($child['error_log']) ? file_get_contents($child['error_log']) : '');
        meminfo_test_cleanup($child);

        return $output;
    }

    function phpinfo_row($name)
    {
        $info = shell_exec(escapeshellarg(meminfo_test_php_binary()) . ' -n -d '
            . escapeshellarg('extension=' . meminfo_test_extension_path()) . ' --ri meminfo');

        return preg_match('/^' . preg_quote($name, '/') . ' => (.*)$/m', $info, $match) ? $match[1] : '-';
    }

    foreach (array('ftp://dumps', 's3://Bad_Bucket', 's3://ab', 's3://dumps/a b', 's3://dumps/../x') as $target) {
        $output = startup_output(array('meminfo.dump_upload' => $target));
        echo $target, ': ', substr_count($output, 'meminfo.dump_upload: '), ' warning, ',
            strpos($output, 'dumps stay local') !== false ? 'dumps stay local' : '?', ', ',
            strpos($output, 'ran') !== false ? 'script ran' : 'script did not run', "\n";
    }

    // A valid target: fine with the S3 transport, reported once without it.
    $s3 = phpinfo_row('S3 transport');
    $canDumpItself = PHP_VERSION_ID >= 70100 && !PHP_ZTS;
    $output = startup_output(array('meminfo.dump_upload' => 's3://dumps/team-a/'));
    $warned = strpos($output, 'meminfo.dump_upload is set but this build') !== false;
    $shouldWarn = !$canDumpItself || strpos($s3, 'yes') !== 0;
    echo 'valid target warns exactly when the build cannot upload: ', $warned === $shouldWarn ? 'yes' : 'no', "\n";

    echo 'S3 transport row: ', strpos($s3, 'yes') === 0 || strpos($s3, 'no (build with --with-meminfo-s3)') === 0 ? 'present' : $s3, "\n";
    echo 'Dump upload row when unset: ', phpinfo_row('Dump upload'), "\n";
?>
--EXPECT--
ftp://dumps: 1 warning, dumps stay local, script ran
s3://Bad_Bucket: 1 warning, dumps stay local, script ran
s3://ab: 1 warning, dumps stay local, script ran
s3://dumps/a b: 1 warning, dumps stay local, script ran
s3://dumps/../x: 1 warning, dumps stay local, script ran
valid target warns exactly when the build cannot upload: yes
S3 transport row: present
Dump upload row when unset: disabled
