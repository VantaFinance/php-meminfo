--TEST--
Fibers (8.1+): the callable of an unstarted fiber and the return value of a finished one are reachable
--SKIPIF--
<?php
    if (PHP_VERSION_ID < 80100) die('skip fibers need PHP 8.1');
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class CapturedByCallable {}
    class ReturnedByFiber {}

    $notStarted = (function () {
        $captured = new CapturedByCallable();

        return new Fiber(function () use ($captured) {
            return $captured;
        });
    })();

    $finished = new Fiber(function () {
        return new ReturnedByFiber();
    });
    $finished->start();

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $items = $meminfoData['items'];
    $roots = [];
    foreach ($items as $id => $item) {
        if (isset($item['symbol_name']) && $item['frame'] === '<GLOBAL>') {
            $roots[$item['symbol_name']] = $id;
        }
    }

    $notStartedChildren = $items[$roots['notStarted']]['children'];
    $callable = $items[$notStartedChildren['[callable]']];
    echo "callable: ", $callable['class'], "\n";
    echo "callable captures: ", $items[$callable['children']['captured']]['class'], "\n";

    $finishedChildren = $items[$roots['finished']]['children'];
    echo "result: ", $items[$finishedChildren['[result]']]['class'], "\n";
    echo "finished fiber keeps no callable: ", var_export(!isset($finishedChildren['[callable]']), true), "\n";
?>
--EXPECT--
callable: Closure
callable captures: CapturedByCallable
result: ReturnedByFiber
finished fiber keeps no callable: true
