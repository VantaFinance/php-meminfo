<?php
/**
 * Helpers for the S3 transport tests (dump-s3_*.phpt).
 *
 * The tests talk to an S3-compatible server named by MEMINFO_TEST_S3_ENDPOINT
 * (make s3-test starts one), with the credentials in AWS_ACCESS_KEY_ID and
 * AWS_SECRET_ACCESS_KEY. Requests are signed here with SigV4 in plain PHP, so
 * the checks work against any server — and without any extension besides
 * hash, as the children run with -n.
 *
 * PHP 7.0-compatible syntax, like signal_helpers.php.
 */

require_once __DIR__ . '/signal_helpers.php';

/**
 * Why the S3 tests cannot run here, or null when they can.
 *
 * @return string|null
 */
function meminfo_test_s3_skip_reason()
{
    if (PHP_VERSION_ID < 70100 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') {
        return 'uploads need a build that dumps on its own (PHP 7.1+, NTS, POSIX)';
    }
    if (!function_exists('proc_open')) {
        return 'proc_open() not available';
    }
    if (strpos(meminfo_test_s3_phpinfo(), 'S3 transport => yes') === false) {
        return 'meminfo built without --with-meminfo-s3';
    }
    if (getenv('MEMINFO_TEST_S3_ENDPOINT') === false || getenv('AWS_ACCESS_KEY_ID') === false) {
        return 'MEMINFO_TEST_S3_ENDPOINT / AWS_ACCESS_KEY_ID not set (make s3-test)';
    }

    return null;
}

/**
 * phpinfo() of the meminfo build under test, from a child: the test runner
 * itself may have loaded another copy.
 *
 * @return string
 */
function meminfo_test_s3_phpinfo()
{
    $command = escapeshellarg(meminfo_test_php_binary()) . ' -n -d '
        . escapeshellarg('extension=' . meminfo_test_extension_path()) . ' --ri meminfo 2>&1';

    return (string) shell_exec($command);
}

/**
 * @return string
 */
function meminfo_test_s3_bucket()
{
    $bucket = getenv('MEMINFO_TEST_S3_BUCKET');

    return $bucket !== false && $bucket !== '' ? $bucket : 'meminfo-test';
}

/**
 * The ini settings that point a child at the test server.
 *
 * @param string $prefix key prefix inside the bucket
 *
 * @return array
 */
function meminfo_test_s3_ini($prefix)
{
    return array(
        'meminfo.dump_upload'    => 's3://' . meminfo_test_s3_bucket() . '/' . $prefix,
        'meminfo.s3_endpoint'    => getenv('MEMINFO_TEST_S3_ENDPOINT'),
        'meminfo.s3_path_style'  => '1',
        'meminfo.s3_region'      => 'us-east-1',
    );
}

/**
 * A key prefix of its own for a test, so runs and tests never see each
 * other's objects.
 *
 * @param string $test
 *
 * @return string
 */
function meminfo_test_s3_prefix($test)
{
    return 'phpt/' . $test . '-' . getmypid() . '-' . mt_rand(100000, 999999);
}

/**
 * A SigV4-signed path-style request to the test server.
 *
 * @param string $method
 * @param string $path   "/bucket" or "/bucket/key", already URL-safe
 * @param string $query  canonical query string, e.g. "list-type=2&prefix=a"
 *
 * @return array {status: int, headers: array, body: string}
 */
function meminfo_test_s3_request($method, $path, $query = '')
{
    $endpoint = rtrim(getenv('MEMINFO_TEST_S3_ENDPOINT'), '/');
    $access = getenv('AWS_ACCESS_KEY_ID');
    $secret = getenv('AWS_SECRET_ACCESS_KEY');
    $region = 'us-east-1';
    $host = parse_url($endpoint, PHP_URL_HOST);
    $port = parse_url($endpoint, PHP_URL_PORT);

    if ($port !== null) {
        $host .= ':' . $port;
    }

    $time = gmdate('Ymd\THis\Z');
    $date = substr($time, 0, 8);
    $payloadHash = hash('sha256', '');

    $canonicalHeaders = 'host:' . $host . "\n"
        . 'x-amz-content-sha256:' . $payloadHash . "\n"
        . 'x-amz-date:' . $time . "\n";
    $signedHeaders = 'host;x-amz-content-sha256;x-amz-date';
    $canonicalRequest = $method . "\n" . $path . "\n" . $query . "\n" . $canonicalHeaders . "\n"
        . $signedHeaders . "\n" . $payloadHash;

    $scope = $date . '/' . $region . '/s3/aws4_request';
    $stringToSign = "AWS4-HMAC-SHA256\n" . $time . "\n" . $scope . "\n" . hash('sha256', $canonicalRequest);

    $key = hash_hmac('sha256', $date, 'AWS4' . $secret, true);
    $key = hash_hmac('sha256', $region, $key, true);
    $key = hash_hmac('sha256', 's3', $key, true);
    $key = hash_hmac('sha256', 'aws4_request', $key, true);
    $signature = hash_hmac('sha256', $stringToSign, $key);

    $headers = array(
        'Authorization: AWS4-HMAC-SHA256 Credential=' . $access . '/' . $scope
            . ', SignedHeaders=' . $signedHeaders . ', Signature=' . $signature,
        'x-amz-content-sha256: ' . $payloadHash,
        'x-amz-date: ' . $time,
    );

    $context = stream_context_create(array(
        'http' => array(
            'method'        => $method,
            'header'        => implode("\r\n", $headers),
            'ignore_errors' => true,
            'timeout'       => 10,
        ),
    ));

    // Headers from the stream's metadata: $http_response_header is deprecated
    // as of PHP 8.5 and http_get_last_response_headers() is 8.4+.
    $stream = @fopen($endpoint . $path . ($query !== '' ? '?' . $query : ''), 'r', false, $context);
    $body = $stream !== false ? stream_get_contents($stream) : false;
    $meta = $stream !== false ? stream_get_meta_data($stream) : array();
    $responseHeaders = isset($meta['wrapper_data']) && is_array($meta['wrapper_data']) ? $meta['wrapper_data'] : array();
    if ($stream !== false) {
        fclose($stream);
    }
    $status = 0;
    $parsed = array();

    foreach ($responseHeaders as $line) {
        if (preg_match('#^HTTP/\S+\s+(\d+)#', $line, $match)) {
            $status = (int) $match[1];
            $parsed = array();
            continue;
        }
        $colon = strpos($line, ':');
        if ($colon !== false) {
            $parsed[strtolower(trim(substr($line, 0, $colon)))] = trim(substr($line, $colon + 1));
        }
    }

    return array('status' => $status, 'headers' => $parsed, 'body' => $body === false ? '' : $body);
}

/**
 * Create the test bucket unless it exists.
 */
function meminfo_test_s3_ensure_bucket()
{
    $path = '/' . meminfo_test_s3_bucket();
    $head = meminfo_test_s3_request('HEAD', $path);

    if ($head['status'] === 200) {
        return;
    }

    $created = meminfo_test_s3_request('PUT', $path);
    if ($created['status'] !== 200 && $created['status'] !== 409) {
        echo 'cannot create the test bucket: HTTP ', $created['status'], ' ', $created['body'], "\n";
    }
}

/**
 * GET an object of the test bucket.
 *
 * @param string $key
 *
 * @return array {status, headers, body}
 */
function meminfo_test_s3_get($key)
{
    return meminfo_test_s3_request('GET', '/' . meminfo_test_s3_bucket() . '/' . $key);
}

/**
 * The location a dump was uploaded to, from its ".uploaded" record, waiting
 * up to $seconds for the upload to finish. Null on failure or timeout; the
 * ".upload-error" record, when there is one, is returned in $error.
 *
 * @param string      $dumpPath
 * @param int         $seconds
 * @param string|null $error
 *
 * @return string|null
 */
function meminfo_test_s3_wait_uploaded($dumpPath, $seconds, &$error = null)
{
    $deadline = microtime(true) + $seconds;
    $error = null;

    while (microtime(true) < $deadline) {
        if (file_exists($dumpPath . '.uploaded')) {
            return trim(file_get_contents($dumpPath . '.uploaded'));
        }
        if (file_exists($dumpPath . '.upload-error')) {
            $error = file_get_contents($dumpPath . '.upload-error');

            return null;
        }
        usleep(50000);
    }

    return null;
}

/**
 * The key of an s3:// URL inside the test bucket.
 *
 * @param string $url
 *
 * @return string
 */
function meminfo_test_s3_key_of($url)
{
    $prefix = 's3://' . meminfo_test_s3_bucket() . '/';

    return strpos($url, $prefix) === 0 ? substr($url, strlen($prefix)) : $url;
}

/**
 * The ini settings that give a child pcntl: none when it is built in, the
 * shared module otherwise, null when there is neither.
 *
 * @return array|null
 */
function meminfo_test_pcntl_ini()
{
    $php = escapeshellarg(meminfo_test_php_binary());
    $probe = ' -r ' . escapeshellarg('echo function_exists("pcntl_fork") ? 1 : 0;') . ' 2>/dev/null';

    if (trim((string) shell_exec($php . ' -n' . $probe)) === '1') {
        return array();
    }
    if (trim((string) shell_exec($php . ' -n -d extension=pcntl.so' . $probe)) === '1') {
        return array('extension' => 'pcntl.so');
    }

    return null;
}
