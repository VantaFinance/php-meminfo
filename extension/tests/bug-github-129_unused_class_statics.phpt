--TEST--
Check that a dump survives classes whose statics were never initialised (GitHub issue #129)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class NeverUsed
    {
        public static $value = array(1, 2, 3);
        protected static $other;

        public static function method()
        {
            static $local;
        }
    }

    class UsedOnce
    {
        public static $value;
    }
    UsedOnce::$value = new stdClass();

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    echo $meminfoData === null ? 'invalid JSON' : 'valid JSON', "\n";
    foreach ($meminfoData['items'] as $item) {
        if ($item['is_root'] && $item['symbol_name'] === 'UsedOnce::value') {
            echo "UsedOnce::value: ", $item['class'], "\n";
        }
    }
?>
--EXPECT--
valid JSON
UsedOnce::value: stdClass
