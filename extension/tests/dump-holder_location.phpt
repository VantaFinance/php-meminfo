--TEST--
Where a holder lives in code: closure function and declaration, frame line of roots, resource type
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class Listener
    {
        public function onEvent()
        {
        }

        public function bound()
        {
            return function () {
            };
        }
    }

    $plain = function () {
    };
    $bound = (new Listener())->bound();
    $fromCallable = (new ReflectionMethod('Listener', 'onEvent'))->getClosure(new Listener());
    $stream = fopen('php://memory', 'r');

    function inFunction()
    {
        $local = array(1);
        $dump = fopen('php://memory', 'rw');
        meminfo_dump($dump);
        rewind($dump);
        $data = json_decode(stream_get_contents($dump), true);
        fclose($dump);

        return $data['items'];
    }

    $items = inFunction();

    $roots = array();
    foreach ($items as $item) {
        if (isset($item['symbol_name']) && !isset($roots[$item['symbol_name']])) {
            $roots[$item['symbol_name']] = $item;
        }
    }

    function where($item)
    {
        return isset($item['declared_at']) ? basename($item['declared_at']) : '-';
    }

    echo "plain: ", $roots['plain']['function'], " at ", where($roots['plain']), "\n";
    echo "bound: ", $roots['bound']['function'], " at ", where($roots['bound']), "\n";
    echo "fromCallable: ", $roots['fromCallable']['function'], " at ", where($roots['fromCallable']), "\n";
    // The line the frame is at: the meminfo_dump() call.
    echo "local line: ", $roots['local']['line'], "\n";
    echo "global root has a line: ", isset($roots['stream']['line']) ? 'yes' : 'no', "\n";
    echo "resource_type: ", $roots['stream']['resource_type'], "\n";
?>
--EXPECTF--
plain: {closure%S} at dump-holder_location.php:%d
bound: Listener::{closure%S} at dump-holder_location.php:%d
fromCallable: Listener::onEvent at dump-holder_location.php:4
local line: 25
global root has a line: yes
resource_type: stream
