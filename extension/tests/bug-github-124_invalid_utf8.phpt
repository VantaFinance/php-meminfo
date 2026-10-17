--TEST--
Check that keys with invalid UTF-8 or NUL bytes still give valid JSON (GitHub issue #124)
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    $keys = array(
        'invalid' => "\xff\xfe",
        'nul' => "a\0b",
        'overlong' => "x\xc0\xafy",
        'surrogate' => "\xed\xa0\x80",
        'truncated' => "end\xe2\x82",
        'valid' => "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x90\x98",
    );

    $myArray = array();
    foreach ($keys as $key) {
        $myArray[$key] = 1;
    }
    $anonymous = new class {
    };

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    if ($meminfoData === null) {
        echo "invalid JSON: ", json_last_error_msg(), "\n";
        exit;
    }

    foreach ($meminfoData['items'] as $item) {
        if (isset($item['symbol_name']) && $item['symbol_name'] == 'myArray') {
            $children = array_keys($item['children']);
        }
        if (isset($item['symbol_name']) && $item['symbol_name'] == 'anonymous') {
            echo "anonymous class: ", $item['class'], "\n";
        }
    }

    foreach ($children as $child) {
        echo bin2hex($child), "\n";
    }
?>
--EXPECT--
anonymous class: class@anonymous
efbfbdefbfbd
610062
78efbfbdefbfbd79
efbfbdefbfbdefbfbd
656e64efbfbdefbfbd
636166c3a920e282ac20f09f9098
