--TEST--
Strings of 32 bytes or more get a "hash" only with the "string_hashes" option, the same for equal strings
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    $first = str_repeat('duplicate-', 5);
    $second = str_repeat('duplicate', 1) . '-' . str_repeat('duplicate-', 4);
    $other = str_repeat('different-', 5);
    $short = str_repeat('s', 31);

    function hashes(array $options)
    {
        $dump = fopen('php://memory', 'rw');
        meminfo_dump($dump, $options);
        rewind($dump);
        $data = json_decode(stream_get_contents($dump), true);
        fclose($dump);

        $hashes = array();
        foreach ($data['items'] as $item) {
            if (isset($item['symbol_name']) && $item['frame'] === '<GLOBAL>' && $item['type'] === 'string') {
                $hashes[$item['symbol_name']] = isset($item['hash']) ? $item['hash'] : null;
            }
        }

        return $hashes;
    }

    $without = hashes(array());
    echo "without the option: ", count(array_filter($without)), " hashes\n";

    $with = hashes(array('string_hashes' => true));
    echo "equal strings, equal hashes: ", var_export($with['first'] !== null && $with['first'] === $with['second'], true), "\n";
    echo "different string, different hash: ", var_export($with['other'] !== $with['first'], true), "\n";
    echo "31 bytes: ", var_export($with['short'], true), "\n";
    echo "hash format: ", preg_match('/^[0-9a-f]{16}$/', $with['first']), "\n";
?>
--EXPECT--
without the option: 0 hashes
equal strings, equal hashes: true
different string, different hash: true
31 bytes: NULL
hash format: 1
