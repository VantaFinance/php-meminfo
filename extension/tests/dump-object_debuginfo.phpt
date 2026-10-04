--TEST--
Check that object properties are dumped without calling __debugInfo
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    $dump = fopen('php://memory', 'rw');

    class MyDebugInfoClass {
        public $realProperty = 'real value';

        public function __debugInfo()
        {
            echo "__debugInfo called\n";

            return ['fakeProperty' => 'fake value'];
        }
    }

    $myObject = new MyDebugInfoClass();

    meminfo_dump($dump);

    rewind($dump);
    $meminfoData = json_decode(stream_get_contents($dump), true);
    fclose($dump);

    $propertyNames = [];
    foreach($meminfoData['items'] as $item) {
        if ($item['type'] === 'object' && $item['class'] === 'MyDebugInfoClass' && isset($item['children'])) {
            $propertyNames = array_keys($item['children']);
        }
    }

    echo "Real property dumped: ".(in_array('realProperty', $propertyNames, true) ? 'yes' : 'no')."\n";
    echo "Fake property dumped: ".(in_array('fakeProperty', $propertyNames, true) ? 'yes' : 'no')."\n";

?>
--EXPECT--
Real property dumped: yes
Fake property dumped: no
