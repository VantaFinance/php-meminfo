--TEST--
threshold capture cleans up traversal bailout and aborts rather than leaving memory_limit lifted
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80100 || PHP_ZTS || DIRECTORY_SEPARATOR === '\\') die('skip refusing memory-limit setter fixture needs PHP 8.1+, NTS, POSIX');
    if (!function_exists('proc_open')) die('skip proc_open required');
    $fixture = getenv('MEMINFO_TEST_FAULT_EXTENSION');
    if (!$fixture || !is_file($fixture)) die('skip test-only fault extension required (OOM engine job)');
    if (!is_dir('/proc/self/fd') || !file_exists('/dev/full')) die('skip Linux write fault fixture required');
?>
--FILE--
<?php
    require __DIR__ . '/fixtures/signal_helpers.php';
    foreach (array('bailout', 'restore', 'write') as $mode) {
        $dir = meminfo_test_make_dump_dir();
        $child = meminfo_test_spawn('
            $marker = oom_fault_create(' . var_export($mode, true) . ');
            $a = array();
            for ($i = 0; $i < 6000; $i++) { $a[] = str_repeat("x", 1024); }
            echo "UNEXPECTED_CONTINUATION\n";
        ', array('extension' => getenv('MEMINFO_TEST_FAULT_EXTENSION'), 'meminfo.dump_dir' => $dir,
            'meminfo.dump_threshold' => '4M', 'memory_limit' => '16M'));
        $status = meminfo_test_finish_child($child);
        $output = file_get_contents($child['stdout']) . file_get_contents($child['stderr']);
        $log = is_file($child['error_log']) ? file_get_contents($child['error_log']) : '';
        echo $mode, "\n";
        if ($mode === 'write') {
            echo 'request continued: ', $status === 0 && strpos($output, 'UNEXPECTED_CONTINUATION') !== false ? 'yes' : 'no', "\n";
        } else {
            echo 'request aborted: ', $status !== 0 && strpos($output, 'UNEXPECTED_CONTINUATION') === false ? 'yes' : 'no', "\n";
        }
        echo 'temporary files: ', count(glob($dir . '/*.tmp')), "\n";
        $dumps = glob($dir . '/*.json');
        echo 'final dumps: ', count($dumps), "\n";
        if ($mode === 'bailout') {
            echo 'cleanup logged: ', strpos($log, 'dump was cut short by a fatal error') !== false ? 'yes' : 'no', "\n";
        } elseif ($mode === 'restore') {
            echo 'failure logged: ', strpos($log, 'cannot restore memory_limit after the dump; aborting request') !== false ? 'yes' : 'no', "\n";
        } else {
            echo 'write failure logged: ', strpos($log, 'meminfo: dump to ') !== false && strpos($log, ' failed: ') !== false ? 'yes' : 'no', "\n";
        }
        meminfo_test_cleanup($child);
        meminfo_test_remove_dir($dir);
    }
?>
--EXPECT--
bailout
request aborted: yes
temporary files: 0
final dumps: 0
cleanup logged: yes
restore
request aborted: yes
temporary files: 0
final dumps: 1
failure logged: yes
write
request continued: yes
temporary files: 0
final dumps: 0
write failure logged: yes
