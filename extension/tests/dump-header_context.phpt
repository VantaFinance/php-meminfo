--TEST--
The header describes the process: PHP version, SAPI, pid, time, memory_limit, RSS, GC status, object handles
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--INI--
memory_limit=256M
--FILE--
<?php
    $before = time();
    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $header = json_decode(stream_get_contents($dump), true)['header'];
    fclose($dump);

    echo "php_version: ", var_export($header['php_version'] === PHP_VERSION, true), "\n";
    echo "sapi: ", $header['sapi'], "\n";
    echo "trigger: ", $header['trigger'], "\n";
    echo "pid: ", var_export(DIRECTORY_SEPARATOR === '\\' || $header['pid'] === getmypid(), true), "\n";
    echo "timestamp: ", var_export($header['timestamp'] >= $before && $header['timestamp'] <= time(), true), "\n";
    echo "memory_limit: ", $header['memory_limit'], "\n";
    echo "rss above memory_usage where known: ", var_export(!isset($header['rss']) || $header['rss'] > $header['memory_usage'], true), "\n";
    echo "rss known on Linux and macOS: ", var_export(!in_array(PHP_OS, array('Linux', 'Darwin'), true) || isset($header['rss']), true), "\n";
    echo "object_handles: ", var_export(is_int($header['object_handles']), true), "\n";
    // gc_status() exists from 7.3: the header carries the same numbers.
    $gc = function_exists('gc_status') ? gc_status() : null;
    echo "gc: ", var_export($gc === null ? !isset($header['gc_runs']) : $header['gc_runs'] === $gc['runs'], true), "\n";
?>
--EXPECT--
php_version: true
sapi: cli
trigger: call
pid: true
timestamp: true
memory_limit: 268435456
rss above memory_usage where known: true
rss known on Linux and macOS: true
object_handles: true
gc: true
