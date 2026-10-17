--TEST--
Check that the dump is laid out without blank lines or stray commas
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class LayoutHolder
    {
        public static $shared = array(1, 2);
        public $name = 'holder';
        private $items = array();
    }

    class LayoutEmpty
    {
    }

    $scalar = 42;
    $emptyArray = array();
    $nested = array('key' => array(1, 2), 'other' => 'value');
    $holder = new LayoutHolder();
    $emptyObject = new LayoutEmpty();
    // Static members are only dumped once the class has initialised them.
    LayoutHolder::$shared[] = 3;

    $dump = fopen('php://memory', 'rw');
    meminfo_dump($dump);
    rewind($dump);
    $text = stream_get_contents($dump);
    fclose($dump);

    $lines = explode("\n", $text);
    $last = array_pop($lines);

    $blank = 0;
    $loneComma = 0;
    $badIndent = array();
    foreach ($lines as $line) {
        if (trim($line) === '') {
            $blank++;
        }
        if (trim($line) === ',') {
            $loneComma++;
        }
        $indent = strlen($line) - strlen(ltrim($line, ' '));
        if (!in_array($indent, array(0, 2, 4, 8, 12), true)) {
            $badIndent[$indent] = true;
        }
    }

    echo "ends with a newline: ", $last === '' ? 'yes' : 'no', "\n";
    echo "blank lines: ", $blank, "\n";
    echo "lines holding only a comma: ", $loneComma, "\n";
    echo "unexpected indentation: ", $badIndent ? implode(',', array_keys($badIndent)) : 'none', "\n";
    echo "closes items and the document: ", substr($text, -strlen("    }\n  }\n}\n")) === "    }\n  }\n}\n" ? 'yes' : 'no', "\n";

    $data = json_decode($text, true);
    echo "valid JSON: ", is_array($data) ? 'yes' : 'no', "\n";

    $bySymbol = array();
    $byClass = array();
    foreach ($data['items'] as $id => $item) {
        if (isset($item['symbol_name'])) {
            $bySymbol[$item['symbol_name']] = $item;
        }
        if (isset($item['class'])) {
            $byClass[$item['class']] = $item;
        }
    }

    // The same fields as before the layout change, with the same types.
    $scalarItem = $bySymbol['scalar'];
    echo "scalar fields: ", implode(',', array_keys($scalarItem)), "\n";
    echo "size is a string: ", is_string($scalarItem['size']) ? 'yes' : 'no', "\n";
    echo "empty array children: ", json_encode($bySymbol['emptyArray']['children']), "\n";
    echo "empty array written as {}: ", strpos($text, '"children" : {}') !== false ? 'yes' : 'no', "\n";
    echo "nested children: ", implode(',', array_keys($bySymbol['nested']['children'])), "\n";
    echo "object fields: ", implode(',', array_keys($byClass['LayoutHolder'])), "\n";
    echo "object children: ", implode(',', array_keys($byClass['LayoutHolder']['children'])), "\n";
    echo "empty object children: ", json_encode($byClass['LayoutEmpty']['children']), "\n";
    echo "static member: ", isset($bySymbol['LayoutHolder::shared']) ? $bySymbol['LayoutHolder::shared']['frame'] : 'missing', "\n";
?>
--EXPECT--
ends with a newline: yes
blank lines: 0
lines holding only a comma: 0
unexpected indentation: none
closes items and the document: yes
valid JSON: yes
scalar fields: type,size,symbol_name,is_root,frame
size is a string: yes
empty array children: []
empty array written as {}: yes
nested children: key,other
object fields: type,size,symbol_name,is_root,frame,class,object_handle,children
object children: name,items
empty object children: []
static member: <CLASS_STATIC_MEMBER>
