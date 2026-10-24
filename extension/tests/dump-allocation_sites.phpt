--TEST--
meminfo.track_allocations records where each object was created with "new"
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--INI--
meminfo.track_allocations=1
--FILE--
<?php
    class Site {}

    function make()
    {
        return new Site();
    }

    $global = new Site();
    $fromFunction = make();
    $fromClosure = call_user_func(function () {
        return new Site();
    });
    $temporary = new Site();
    unset($temporary);
    // Takes the freed handle without "new": no site, and not the old one.
    $unserialized = unserialize('O:8:"stdClass":0:{}');

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $items = json_decode(stream_get_contents($dump), true)['items'];
    fclose($dump);

    foreach ($items as $item) {
        if (isset($item['symbol_name'], $item['class']) && $item['frame'] === '<GLOBAL>') {
            echo $item['symbol_name'], ": ", isset($item['site']) ? basename($item['site']) : 'none', "\n";
        }
    }
?>
--EXPECT--
global: dump-allocation_sites.php:9
fromFunction: dump-allocation_sites.php:6
fromClosure: dump-allocation_sites.php:12
unserialized: none
