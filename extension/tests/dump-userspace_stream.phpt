--TEST--
Check that a userspace stream wrapper gets the dump only after the heap walk
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class RecordingStream
    {
        public static $data = '';
        public static $writes = 0;
        public $context;

        public function stream_open($path, $mode, $options, &$opened_path)
        {
            return true;
        }

        public function stream_write($data)
        {
            // Runs PHP code and mutates the heap: harmless only once the walk
            // is over.
            self::$data .= $data;
            self::$writes++;
            // Not through $GLOBALS: on PHP 7 using it turns every global into
            // a child of that array before it can be a root.
            global $churn;
            $churn[] = str_repeat('x', 100);
            return strlen($data);
        }
    }

    stream_wrapper_register('recording', 'RecordingStream');

    $churn = array();
    $bulk = range(1, 20000);

    $stream = fopen('recording://dump', 'w');
    meminfo_dump($stream);
    fclose($stream);

    $meminfoData = json_decode(RecordingStream::$data, true);
    echo $meminfoData === null ? 'invalid JSON' : 'valid JSON', "\n";

    foreach ($meminfoData['items'] as $item) {
        if (isset($item['symbol_name']) && $item['symbol_name'] === 'bulk') {
            echo "bulk: ", count($item['children']), " children\n";
        }
        if (isset($item['symbol_name']) && $item['symbol_name'] === 'churn') {
            echo "churn at dump time: ", count($item['children']), " children\n";
        }
    }
    echo RecordingStream::$writes > 0 ? 'written' : 'not written', "\n";
?>
--EXPECT--
valid JSON
churn at dump time: 0 children
bulk: 20000 children
written
