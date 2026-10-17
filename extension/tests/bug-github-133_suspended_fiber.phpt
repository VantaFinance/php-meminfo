--TEST--
Check that an object held only by a suspended Fiber is dumped once, under the fiber's frame (GitHub issue #133)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (PHP_VERSION_ID < 80100) die('skip Fibers need PHP 8.1');
?>
--FILE--
<?php
    class FiberLocal {}

    $fiber = new Fiber(function () {
        $held = new ArrayObject(array(new FiberLocal()));
        Fiber::suspend();
        return $held;
    });
    $fiber->start();

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $count = 0;
    foreach ($meminfoData['items'] as $item) {
        if (isset($item['class']) && $item['class'] === 'FiberLocal') {
            $count++;
        }
        if (isset($item['symbol_name']) && $item['symbol_name'] === 'held') {
            echo "held: ", $item['class'], " in ", $item['frame'], ", ", count($item['children']), " child\n";
        }
    }
    echo "FiberLocal: $count\n";

    $fiber->resume();
?>
--EXPECTF--
held: ArrayObject in {closure%S}(), 1 child
FiberLocal: 1
