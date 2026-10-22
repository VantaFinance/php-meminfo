--TEST--
Check that static variables of functions and methods are dumped (GitHub issue #75)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class FunctionStatic {}
    class MethodStatic {}

    function remember()
    {
        static $kept;
        $kept = new FunctionStatic();
    }

    function neverCalled()
    {
        static $unused = array();
    }

    class Registry
    {
        public function remember()
        {
            static $cache = array();
            $cache[] = new MethodStatic();
        }
    }

    remember();
    (new Registry())->remember();

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $items = $meminfoData['items'];
    $roots = array();
    foreach ($items as $item) {
        if ($item['is_root'] && $item['frame'] === '<STATIC_VARIABLE>') {
            $roots[] = $item;
        }
    }
    usort($roots, function ($a, $b) {
        return strcmp($a['symbol_name'], $b['symbol_name']);
    });

    foreach ($roots as $root) {
        $type = isset($root['class']) ? $root['class'] : $root['type'];
        echo $root['symbol_name'], ': ', $type, "\n";
    }

    foreach ($items as $item) {
        if (isset($item['class']) && $item['class'] === 'MethodStatic') {
            echo "MethodStatic dumped\n";
        }
    }
?>
--EXPECT--
Registry::remember()::$cache: array
remember()::$kept: FunctionStatic
MethodStatic dumped
