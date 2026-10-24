--TEST--
meminfo.track_allocations records the "new" of an object created inside a fiber (8.1+)
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80100) die('skip fibers need PHP 8.1');
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--INI--
meminfo.track_allocations=1
--FILE--
<?php
    class MadeInFiber {}

    $fiber = new Fiber(function () {
        return new MadeInFiber();
    });
    $fiber->start();
    $made = $fiber->getReturn();

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    foreach (json_decode(stream_get_contents($dump), true)['items'] as $item) {
        if (isset($item['class']) && $item['class'] === 'MadeInFiber') {
            echo "site: ", basename($item['site']), "\n";
        }
    }
?>
--EXPECT--
site: dump-allocation_sites_fiber.php:5
