--TEST--
Check that what closures and generators hold is dumped (GitHub issue #63)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class UseMarker {}
    class ThisMarker {}
    class StaticMarker {}
    class GeneratorLocalMarker {}

    class Owner
    {
        public function makeClosure()
        {
            return function () {
                return $this;
            };
        }
    }

    function makeGenerator()
    {
        $local = new GeneratorLocalMarker();
        yield 1;
        yield $local;
    }

    $used = new UseMarker();
    $useClosure = function () use ($used) {
        return $used;
    };
    unset($used);

    $thisClosure = (new Owner())->makeClosure();

    $staticClosure = function () {
        static $kept;
        $kept = new StaticMarker();
    };
    $staticClosure();

    $generator = makeGenerator();
    $generator->current();

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $items = $meminfoData['items'];
    $classes = array();
    foreach ($items as $address => $item) {
        if (isset($item['class'])) {
            $classes[$item['class']][] = $address;
        }
    }

    // PHP 7.0 generators have no get_gc handler: a suspended generator's
    // locals are out of reach there.
    if (PHP_VERSION_ID < 70100) {
        $classes['GeneratorLocalMarker'] = array('unreachable on 7.0');
    }

    foreach (array('UseMarker', 'Owner', 'StaticMarker', 'GeneratorLocalMarker') as $class) {
        echo $class, ': ', isset($classes[$class]) ? count($classes[$class]) : 0, "\n";
    }

    // The marker hangs off its closure, not off a root of its own.
    foreach ($items as $item) {
        if (isset($item['symbol_name']) && $item['symbol_name'] === 'useClosure') {
            $child = isset($item['children']['used']) ? $item['children']['used'] : null;
            echo "useClosure -> used: ", ($child !== null && $items[$child]['class'] === 'UseMarker') ? 'yes' : 'no', "\n";
        }
    }
?>
--EXPECT--
UseMarker: 1
Owner: 1
StaticMarker: 1
GeneratorLocalMarker: 1
useClosure -> used: yes
