--TEST--
Item sizes are the allocations they stand for (size_model 2)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class ThreeProperties
    {
        public $a;
        public $b;
        public $c;
    }

    function string_size($length)
    {
        // Header (refcount and flags, hash, length), characters, NUL; 8-aligned.
        return (24 + $length + 1 + 7) & ~7;
    }

    $packed = range(1, 100);
    $hash = array();
    foreach (array('alpha', 'beta', 'gamma') as $key) {
        $hash[str_repeat($key, 3)] = 1;
    }
    $string = str_repeat('x', 1000);
    $object = new ThreeProperties();
    $closure = function () {};
    $int = 42;
    $literal = 'interned literal';

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $roots = array();
    foreach ($meminfoData['items'] as $item) {
        if (isset($item['symbol_name']) && $item['frame'] === '<GLOBAL>') {
            $roots[$item['symbol_name']] = $item;
        }
    }

    echo "size_model: ", $meminfoData['header']['size_model'], "\n";

    // Packed: header, slots (zvals from 8.2, buckets before) and an 8-byte hash.
    $slot = PHP_VERSION_ID >= 80200 ? 16 : 32;
    $p = $roots['packed'];
    echo "packed: ", var_export((int) $p['size'] === 56 + $p['capacity'] * $slot + 8, true), " (used ", $p['used'], ")\n";

    // Hash: header, 32-byte buckets plus 4-byte hash slots (two per bucket
    // from 7.3, one before), and the keys.
    $h = $roots['hash'];
    $keys = string_size(15) + string_size(12) + string_size(15);
    $perBucket = PHP_VERSION_ID >= 70300 ? 40 : 36;
    echo "hash: ", var_export((int) $h['size'] === 56 + $h['capacity'] * $perBucket + $keys, true), "\n";

    echo "string: ", var_export((int) $roots['string']['size'] === string_size(1000), true), "\n";

    // zend_object holds the first property slot itself.
    echo "object: ", var_export((int) $roots['object']['size'] === 56 + 2 * 16, true), "\n";
    echo "closure larger than a plain object: ", var_export((int) $roots['closure']['size'] > 200, true), "\n";
    echo "int: ", $roots['int']['size'], "\n";
    echo "interned literal: ", $roots['literal']['size'], "\n";
?>
--EXPECT--
size_model: 2
packed: true (used 100)
hash: true
string: true
object: true
closure larger than a plain object: true
int: 0
interned literal: 0
