--TEST--
Memory outside zvals: output buffers, memory and temp stream content, VM stack and compiled code in the header
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class CompiledHere {}
    function compiledHere() {}

    $memory = fopen('php://memory', 'w+');
    fwrite($memory, str_repeat('m', 1 << 20));
    $temp = fopen('php://temp', 'w+');
    fwrite($temp, str_repeat('t', 100000));

    ob_start();
    echo str_repeat('o', 1 << 20);
    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    ob_end_clean();
    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $roots = array();
    $buffers = array();
    foreach ($meminfoData['items'] as $item) {
        if (isset($item['symbol_name']) && $item['frame'] === '<GLOBAL>') {
            $roots[$item['symbol_name']] = $item;
        }
        if ($item['type'] === 'buffer') {
            $buffers[] = $item;
        }
    }

    echo "output buffer item: ", count($buffers), " ", $buffers[0]['frame'], " ", $buffers[0]['symbol_name'], "\n";
    echo "output buffer holds 1 MB: ", var_export((int) $buffers[0]['size'] >= 1 << 20, true), "\n";
    echo "php://memory holds 1 MB: ", var_export((int) $roots['memory']['size'] >= 1 << 20, true), "\n";
    echo "php://temp holds 100 KB: ", var_export((int) $roots['temp']['size'] >= 100000, true), "\n";

    $header = $meminfoData['header'];
    echo "vm_stack_size: ", var_export($header['vm_stack_size'] > 0, true), "\n";
    echo "class_count: ", var_export($header['class_count'] > $header['user_class_count'], true), "\n";
    echo "user classes and functions: ", var_export($header['user_class_count'] >= 1 && $header['user_function_count'] >= 1, true), "\n";
?>
--EXPECT--
output buffer item: 1 <OUTPUT_BUFFER> ob_start[0]
output buffer holds 1 MB: true
php://memory holds 1 MB: true
php://temp holds 100 KB: true
vm_stack_size: true
class_count: true
user classes and functions: true
