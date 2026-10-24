--TEST--
What a frame holds besides its variables: $this, the running closure, extra and internal-call arguments, live temporaries
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class LoopItem {}
    class Runner {}
    class MappedItem {}
    class ExtraArgument {}
    class CapturedByClosure {}

    function dump_items()
    {
        $dump = fopen('php://memory', 'rw');
        meminfo_dump($dump);
        rewind($dump);
        $data = json_decode(stream_get_contents($dump), true);
        fclose($dump);

        return $data['items'];
    }

    function count_class(array $items, $class)
    {
        $count = 0;
        foreach ($items as $item) {
            if (isset($item['class']) && $item['class'] === $class) {
                $count++;
            }
        }

        return $count;
    }

    function root_classes(array $items, $symbol)
    {
        $classes = array();
        foreach ($items as $item) {
            if ($item['is_root'] && isset($item['symbol_name']) && $item['symbol_name'] === $symbol && isset($item['class'])) {
                $classes[] = $item['class'];
            }
        }
        sort($classes);

        return implode(',', $classes);
    }

    function load()
    {
        return array(new LoopItem(), new LoopItem(), new LoopItem());
    }

    // Held only by the copy foreach iterates over.
    foreach (load() as $index => $item) {
        if ($index === 0) {
            $items = dump_items();
            // Live temporaries are read from 7.1 on; 7.0 sees only $item.
            echo "foreach copy: ", count_class($items, 'LoopItem') === (PHP_VERSION_ID >= 70100 ? 3 : 1) ? 'ok' : 'missing', "\n";
        }
    }
    unset($item);

    class Runner2 extends Runner
    {
        public function run()
        {
            return dump_items();
        }
    }

    // A temporary receiver: held only by the frame of run().
    $items = (new Runner2())->run();
    echo "\$this: ", root_classes($items, '$this'), "\n";

    // The array passed to array_map() is held only by array_map()'s frame.
    array_map(function ($item) {
        static $done = false;
        if (!$done) {
            $done = true;
            $items = dump_items();
            echo "array_map() argument: ", count_class($items, 'MappedItem'), " MappedItem\n";
        }
    }, array(new MappedItem(), new MappedItem()));

    function takesOne($one)
    {
        return dump_items();
    }

    $items = takesOne(1, new ExtraArgument());
    echo "[arg 1]: ", root_classes($items, '[arg 1]'), "\n";

    $items = (function () {
        $captured = null;
        return function () use (&$captured) {
            $captured = new CapturedByClosure();
            return dump_items();
        };
    })()();
    echo "{closure}: ", root_classes($items, '{closure}'), "\n";
    echo "captured: ", count_class($items, 'CapturedByClosure'), "\n";
?>
--EXPECT--
foreach copy: ok
$this: Runner2
array_map() argument: 2 MappedItem
[arg 1]: ExtraArgument
{closure}: Closure
captured: 1
