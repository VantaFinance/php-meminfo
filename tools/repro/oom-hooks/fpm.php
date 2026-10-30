<?php
/** Repeated-request and immediate-OOM worker lifecycle in the pinned FPM image. */
require __DIR__ . '/../../../extension/tests/fixtures/signal_helpers.php';

function oom_fpm_check($condition, $label)
{
    if (!$condition) {
        throw new RuntimeException('error: ' . $label);
    }
    echo 'PASS ', $label, "\n";
}

function oom_fpm_request($work, $script)
{
    $prefix = $work . '/request-' . mt_rand(100000, 999999);
    $child = array('script' => $script, 'stdout' => $prefix . '.out', 'stderr' => $prefix . '.err');
    $command = 'SCRIPT_FILENAME=' . escapeshellarg($work . '/' . $script)
        . ' SCRIPT_NAME=/' . escapeshellarg($script)
        . ' REQUEST_METHOD=GET cgi-fcgi -bind -connect ' . escapeshellarg($work . '/fpm.sock');
    $child['process'] = proc_open($command, array(0 => array('file', '/dev/null', 'r'),
        1 => array('file', $child['stdout'], 'w'), 2 => array('file', $child['stderr'], 'w')), $pipes);
    $status = meminfo_test_finish_child($child, 25);
    $body = file_get_contents($child['stdout']);
    $parts = preg_split('/\r?\n\r?\n/', $body, 2);
    return array('status' => $status, 'data' => count($parts) === 2 ? json_decode($parts[1], true) : null,
        'raw' => $body . file_get_contents($child['stderr']));
}

$binary = getenv('MEMINFO_FPM_BINARY');
if (!$binary || !is_executable($binary)) {
    throw new RuntimeException('error: pinned FPM runtime required');
}
$native = meminfo_test_native_oom_behavior();
echo json_encode(array('fpm_binary' => $binary, 'fpm_sha256' => hash_file('sha256', $binary),
    'cli_php' => PHP_VERSION, 'native_control' => $native)), "\n";
$work = meminfo_test_make_dump_dir();
mkdir($work . '/dumps', 0700);
file_put_contents($work . '/normal.php', '<?php
    $a = array();
    for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
    unset($a);
    echo json_encode(array("pid" => getmypid(), "reclaimed" => gc_mem_caches() > 0));
');
file_put_contents($work . '/fatal.php', '<?php $a = array(); while (true) { $a[] = str_repeat("x", 1024); }');
$config = "[global]\ndaemonize = no\nerror_log = $work/fpm.log\n"
    . "[test]\nuser = root\ngroup = root\nlisten = $work/fpm.sock\npm = static\npm.max_children = 1\n"
    . "catch_workers_output = yes\nphp_admin_value[error_log] = $work/request.log\n";
file_put_contents($work . '/fpm.conf', $config);
$command = escapeshellarg($binary) . ' -R -F -n -y ' . escapeshellarg($work . '/fpm.conf')
    . ' -d ' . escapeshellarg('extension=' . meminfo_test_extension_path())
    . ' -d memory_limit=16M -d meminfo.dump_threshold=4M -d ' . escapeshellarg('meminfo.dump_dir=' . $work . '/dumps');
$fpm = array('script' => $binary, 'stdout' => $work . '/master.out', 'stderr' => $work . '/master.err');
$fpm['process'] = proc_open($command, array(0 => array('file', '/dev/null', 'r'),
    1 => array('file', $fpm['stdout'], 'w'), 2 => array('file', $fpm['stderr'], 'w')), $pipes);
try {
    $deadline = microtime(true) + 10;
    while (!file_exists($work . '/fpm.sock') && microtime(true) < $deadline) { usleep(10000); }
    oom_fpm_check(file_exists($work . '/fpm.sock'), 'FPM started');
    $pid = null;
    for ($i = 0; $i < 3; $i++) {
        $response = oom_fpm_request($work, 'normal.php');
        $data = $response['data'];
        oom_fpm_check($response['status'] === 0 && isset($data['pid']) && $data['reclaimed'], 'request completed with native GC');
        if ($pid === null) { $pid = $data['pid']; }
        oom_fpm_check($data['pid'] === $pid, 'same worker reused');
        oom_fpm_check(count(glob($work . '/dumps/*.json')) === $i + 1, 'threshold state reset for next request');
    }
    $fatal = oom_fpm_request($work, 'fatal.php');
    oom_fpm_check(count(glob($work . '/dumps/*.json')) === 4, 'early dump finalized before fatal');
    $response = oom_fpm_request($work, 'normal.php');
    $data = $response['data'];
    oom_fpm_check($response['status'] === 0 && isset($data['pid']) && $data['reclaimed'], 'pool serves request after fatal');
    oom_fpm_check($native['immediate_exit'] ? $data['pid'] !== $pid : $data['pid'] === $pid,
        'worker lifecycle agrees with native OOM class');
    oom_fpm_check(count(glob($work . '/dumps/*.json')) === 5, 'replacement or surviving worker captures next request');
    foreach (glob($work . '/dumps/*.json') as $path) {
        $data = json_decode(file_get_contents($path), true);
        oom_fpm_check(isset($data['header']['trigger']) && $data['header']['trigger'] === 'threshold'
            && !empty($data['items']), 'valid threshold dump');
    }
} finally {
    proc_terminate($fpm['process']);
    meminfo_test_finish_child($fpm, 10);
    // Retain evidence inside the container for CI artifact collection.
    echo 'FPM artifacts: ', $work, "\n";
}
