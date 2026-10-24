--TEST--
With meminfo.dump_defer=1 a dump signal waits for meminfo_dump_if_requested()
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (PHP_VERSION_ID < 70100) die('skip signal dumps need PHP 7.1 or later');
    if (PHP_ZTS) die('skip signal dumps are not available on ZTS builds');
    if (DIRECTORY_SEPARATOR === '\\') die('skip signal dumps are POSIX only');
    if (!function_exists('proc_open')) die('skip proc_open() is required');
?>
--INI--
meminfo.dump_defer=1
--FILE--
<?php
    require dirname(__FILE__) . '/fixtures/signal_helpers.php';

    $dumpDir = sys_get_temp_dir();
    $pattern = meminfo_test_self_dump_pattern();

    class BetweenJobsMarker {}
    $kept = new BetweenJobsMarker();

    echo "nothing pending: ", var_export(meminfo_dump_if_requested(), true), "\n";

    // The signal arrives mid-job: nothing is written while the job runs.
    meminfo_test_raise_signal();
    $job = array();
    for ($i = 0; $i < 200000; $i++) {
        $job[] = $i;
    }
    echo "dump written during the job: ", meminfo_test_wait_for_dump($dumpDir, $pattern, 1) === null ? 'no' : 'yes', "\n";
    unset($job);

    // Between two jobs: the worker loop asks for the pending dump.
    $path = meminfo_dump_if_requested();
    echo "dump written between jobs: ", is_string($path) && is_file($path) ? 'yes' : 'no', "\n";
    if (is_string($path)) {
        $items = meminfo_test_items($path);
        echo "marker present: ", meminfo_test_has_class($items, 'BetweenJobsMarker') ? 'yes' : 'no', "\n";
        echo "trigger: ", json_decode(file_get_contents($path), true)['header']['trigger'], "\n";
        unlink($path);
    }

    echo "served once: ", var_export(meminfo_dump_if_requested(), true), "\n";
?>
--EXPECT--
nothing pending: NULL
dump written during the job: no
dump written between jobs: yes
marker present: yes
trigger: deferred
served once: NULL
