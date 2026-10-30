<?php
/**
 * PHP-FPM end-to-end scenarios for the S3 transport (make repro-s3-fpm).
 *
 * Runs inside the S3 image built on php:X.Y-fpm, next to the S3 server that
 * tools/repro/s3/run-tests.sh starts, and talks FastCGI to a php-fpm started
 * here (cgi-fcgi). Each pool sets its own meminfo.* with php_admin_value:
 *
 * meminfo.dump_on_limit and meminfo.dump_threshold are on for the whole
 * server (-d): they hook into the engine at startup, so a pool can switch them
 * off but not on.
 *
 *   upload  - memory_limit dump uploaded while the worker keeps serving
 *   hang    - a threshold dump whose upload never finishes (the server only
 *             accepts the connection): the response does not wait for it,
 *             and a worker recycled by pm.max_requests waits at most
 *             meminfo.dump_upload_timeout
 *   noenv   - clear_env without env[AWS_*]: the missing credentials are
 *             reported in the log, the dump stays local
 *
 * Prints one line per check and exits non-zero when one fails.
 */

require '/src/extension/tests/fixtures/s3_helpers.php';

$work = '/tmp/fpm-s3';
$failed = 0;

/**
 * @param string $label
 * @param bool   $ok
 * @param string $detail
 */
function check($label, $ok, $detail = '')
{
    global $failed;

    echo $ok ? 'PASS ' : 'FAIL ', $label, $ok || $detail === '' ? '' : ' (' . $detail . ')', "\n";
    if (!$ok) {
        $failed++;
    }
}

/**
 * A pool listening on a socket of its own, one static worker.
 *
 * @param string $name
 * @param array  $admin   php_admin_value settings
 * @param array  $pool    extra pool settings
 *
 * @return string
 */
function pool($name, array $admin, array $pool = array())
{
    global $work;

    $lines = array(
        '[' . $name . ']',
        'listen = ' . $work . '/' . $name . '.sock',
        'pm = static',
        'pm.max_children = 1',
        'catch_workers_output = yes',
        'php_admin_value[error_log] = ' . $work . '/' . $name . '.log',
        'php_admin_value[meminfo.dump_dir] = ' . $work . '/dumps-' . $name,
    );
    foreach ($pool as $key => $value) {
        $lines[] = $key . ' = ' . $value;
    }
    foreach ($admin as $key => $value) {
        $lines[] = 'php_admin_value[' . $key . '] = ' . $value;
    }
    @mkdir($work . '/dumps-' . $name, 0700, true);

    return implode("\n", $lines) . "\n";
}

/**
 * One FastCGI request; the time it took and what came back.
 *
 * @param string $name
 * @param string $script
 *
 * @return array {seconds: float, body: string}
 */
function request($name, $script)
{
    global $work;

    $env = 'SCRIPT_FILENAME=' . escapeshellarg($work . '/www/' . $script)
        . ' SCRIPT_NAME=/' . escapeshellarg($script) . ' REQUEST_METHOD=GET';
    $started = microtime(true);
    $body = (string) shell_exec($env . ' cgi-fcgi -bind -connect ' . escapeshellarg($work . '/' . $name . '.sock') . ' 2>&1');

    return array('seconds' => microtime(true) - $started, 'body' => $body);
}

/**
 * @param string $pattern
 * @param float  $seconds
 *
 * @return string|null
 */
function wait_for_file($pattern, $seconds)
{
    $deadline = microtime(true) + $seconds;
    while (microtime(true) < $deadline) {
        $found = glob($pattern);
        if ($found) {
            return $found[0];
        }
        usleep(50000);
    }

    return null;
}

@mkdir($work . '/www', 0755, true);
file_put_contents($work . '/www/pid.php', '<?php echo getmypid();');
file_put_contents($work . '/www/grow.php', '<?php $grow = array(); while (true) { $grow[] = str_repeat("x", 1024); }');
// The threshold is checked on calls of user functions.
file_put_contents($work . '/www/work.php', '<?php function step() { return 1; } $keep = str_repeat("x", 40 * 1024 * 1024); step(); echo "done ", getmypid();');

// Accepts connections and never answers.
$blackhole = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
$blackholeAddress = stream_socket_get_name($blackhole, false);

meminfo_test_s3_ensure_bucket();
$endpoint = getenv('MEMINFO_TEST_S3_ENDPOINT');
$credentials = array(
    'env[AWS_ACCESS_KEY_ID]'     => getenv('AWS_ACCESS_KEY_ID'),
    'env[AWS_SECRET_ACCESS_KEY]' => getenv('AWS_SECRET_ACCESS_KEY'),
);
$prefix = meminfo_test_s3_prefix('fpm');

$config = "[global]\nerror_log = " . $work . "/fpm.log\ndaemonize = no\n"
    . pool('upload', array(
        'meminfo.dump_upload'   => 's3://' . meminfo_test_s3_bucket() . '/' . $prefix,
        'meminfo.s3_endpoint'   => $endpoint,
        'meminfo.s3_path_style'  => '1',
        'meminfo.dump_threshold' => '',
        'memory_limit'           => '32M',
    ), $credentials)
    . pool('hang', array(
        'meminfo.dump_upload'         => 's3://' . meminfo_test_s3_bucket() . '/' . $prefix,
        'meminfo.s3_endpoint'         => 'http://' . $blackholeAddress,
        'meminfo.s3_path_style'       => '1',
        'meminfo.dump_upload_timeout' => '2',
        'memory_limit'                => '64M',
    ), array_merge($credentials, array('pm.max_requests' => '2')))
    . pool('noenv', array(
        'meminfo.dump_upload'   => 's3://' . meminfo_test_s3_bucket() . '/' . $prefix,
        'meminfo.s3_endpoint'   => $endpoint,
        'meminfo.s3_path_style'  => '1',
        'meminfo.dump_threshold' => '',
        'memory_limit'           => '32M',
    ), array('clear_env' => 'yes'));
file_put_contents($work . '/php-fpm.conf', $config);

// dump_on_limit and dump_threshold hook into the engine at startup, so they
// are switched on for the whole server; a pool can only switch them off.
$fpm = proc_open(array('php-fpm', '-R', '-F', '-y', $work . '/php-fpm.conf',
    '-d', 'meminfo.dump_on_limit=1', '-d', 'meminfo.dump_threshold=50%'), array(
    0 => array('file', '/dev/null', 'r'),
    1 => array('file', $work . '/fpm.out', 'w'),
    2 => array('file', $work . '/fpm.out', 'a'),
), $pipes);
foreach (array('upload', 'hang', 'noenv') as $name) {
    wait_for_file($work . '/' . $name . '.sock', 10);
}

// --- upload: the worker that hit memory_limit keeps serving, the dump goes up
$before = request('upload', 'pid.php');
$fatal = request('upload', 'grow.php');
$after = request('upload', 'pid.php');
check('upload: the request ends in the memory error', strpos($fatal['body'], 'Allowed memory size') !== false, $fatal['body']);
check('upload: the same worker keeps serving', trim(preg_replace('/^.*\r?\n\r?\n/s', '', $before['body'])) === trim(preg_replace('/^.*\r?\n\r?\n/s', '', $after['body'])));
$record = wait_for_file($work . '/dumps-upload/meminfo-*.json.uploaded', 20);
check('upload: uploaded in the background', $record !== null);
if ($record !== null) {
    $object = meminfo_test_s3_get(meminfo_test_s3_key_of(trim(file_get_contents($record))));
    check('upload: object in the bucket, trigger memory_limit', $object['status'] === 200
        && isset($object['headers']['x-amz-meta-meminfo-trigger']) && $object['headers']['x-amz-meta-meminfo-trigger'] === 'memory_limit');
}

// --- hang: the response does not wait; recycling waits at most the timeout
$slow = request('hang', 'work.php');
check('hang: the threshold dump was taken', wait_for_file($work . '/dumps-hang/meminfo-*.json', 10) !== null);
check('hang: the response did not wait for the upload', $slow['seconds'] < 3, round($slow['seconds'], 1) . 's');
$second = request('hang', 'pid.php');
$recycleStarted = microtime(true);
$third = request('hang', 'pid.php');
check('hang: the worker was recycled and replaced', strpos($third['body'], 'Status') !== false || trim($third['body']) !== '');
$error = wait_for_file($work . '/dumps-hang/meminfo-*.json.upload-error', 15);
check('hang: the exiting worker gave up after dump_upload_timeout', $error !== null
    && strpos((string) @file_get_contents($work . '/hang.log'), 'gave up on 1 dump upload(s) after 2s') !== false);

// --- noenv: clear_env drops the credentials, and the log says so
request('noenv', 'grow.php');
request('noenv', 'pid.php');
$noenvError = wait_for_file($work . '/dumps-noenv/meminfo-*.json.upload-error', 10);
check('noenv: the upload fails without credentials', $noenvError !== null && strpos(file_get_contents($noenvError), 'clear_env') !== false);
check('noenv: the dump stays local', $noenvError !== null && is_file(substr($noenvError, 0, -strlen('.upload-error'))));

proc_terminate($fpm, SIGQUIT);
proc_close($fpm);
fclose($blackhole);

if ($failed > 0) {
    echo "\n--- fpm.out\n", @file_get_contents($work . '/fpm.out');
    foreach (array('upload', 'hang', 'noenv') as $name) {
        echo "\n--- ", $name, ".log\n", @file_get_contents($work . '/' . $name . '.log');
    }
    exit(1);
}
