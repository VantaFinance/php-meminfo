--TEST--
Check that user constants holding values of their own are dumped, and those of PHP are not
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    define('RUNTIME_LIST', array_fill(0, 3, str_repeat('x', 3)));
    define('RUNTIME_TEXT', str_repeat('y', 64));
    define('PLAIN_NUMBER', 42);

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $constants = array();
    foreach ($meminfoData['items'] as $item) {
        if ($item['is_root'] && $item['frame'] === '<CONSTANT>') {
            $constants[] = $item['symbol_name'] . ': ' . $item['type'];
        }
    }
    sort($constants);
    echo implode("\n", $constants), "\n";
?>
--EXPECT--
RUNTIME_LIST: array
RUNTIME_TEXT: string
