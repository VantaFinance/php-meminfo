--TEST--
Check that objects whose get_properties handler returns NULL (ext-protobuf) produce valid JSON
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (!extension_loaded('protobuf')) die('skip protobuf ext not loaded');
?>
--FILE--
<?php
    $timestamp = new Google\Protobuf\Timestamp();
    $timestamp->setSeconds(1700000000);

    $value = new Google\Protobuf\Value();
    $value->setStringValue('hello');

    $struct = new Google\Protobuf\Struct();
    $structFields = $struct->getFields();
    $structFields['key'] = $value;

    $listValue = new Google\Protobuf\ListValue();
    $listValues = $listValue->getValues();

    $dump = fopen('php://memory', 'rw');

    meminfo_dump($dump);

    rewind($dump);
    $text = stream_get_contents($dump);
    $meminfoData = json_decode($text, true);
    fclose($dump);

    // The empty children of these objects are written by a line of their own.
    echo "blank lines: ", preg_match_all('/\n[ ]*\n/', $text), "\n";

    if (is_array($meminfoData)) {
        echo "meminfo_dump JSON decode ok\n";
    } else {
        echo "meminfo_dump JSON decode fail: ".json_last_error_msg()."\n";
        exit;
    }

    $classes = [];
    foreach ($meminfoData['items'] as $item) {
        if ($item['type'] === 'object' && strpos($item['class'], 'Google\\Protobuf\\') === 0) {
            // protobuf 4.30 moved RepeatedField (and may move MapField) out of Internal\.
            $class = str_replace(
                ['Google\\Protobuf\\Internal\\RepeatedField', 'Google\\Protobuf\\Internal\\MapField'],
                ['Google\\Protobuf\\RepeatedField', 'Google\\Protobuf\\MapField'],
                $item['class']
            );
            $classes[] = $class.': '.(isset($item['children']) && $item['children'] === [] ? 'empty children' : 'unexpected children');
        }
    }
    sort($classes);

    echo implode("\n", $classes)."\n";
?>
--EXPECT--
blank lines: 0
meminfo_dump JSON decode ok
Google\Protobuf\ListValue: empty children
Google\Protobuf\MapField: empty children
Google\Protobuf\RepeatedField: empty children
Google\Protobuf\Struct: empty children
Google\Protobuf\Timestamp: empty children
Google\Protobuf\Value: empty children
