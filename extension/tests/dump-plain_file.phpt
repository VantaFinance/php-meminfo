--TEST--
Check that a dump into a plain file leaves the stream position where the dump ended
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    $path = __DIR__ . '/dump-plain_file.json.tmp';
    $myArray = array('a' => 1);

    $stream = fopen($path, 'w+');
    fwrite($stream, 'prefix:');
    meminfo_dump($stream);
    $end = ftell($stream);
    fwrite($stream, ':suffix');
    fclose($stream);

    $text = file_get_contents($path);
    unlink($path);

    echo strpos($text, 'prefix:{') === 0 ? 'prefix kept' : 'prefix lost', "\n";
    echo $end === strlen($text) - strlen(':suffix') ? 'position follows the dump' : "position $end", "\n";
    echo substr($text, -7) === ':suffix' ? 'suffix appended' : 'suffix lost', "\n";

    $meminfoData = json_decode(substr($text, 7, -7), true);
    echo $meminfoData === null ? 'invalid JSON' : 'valid JSON', "\n";
?>
--EXPECT--
prefix kept
position follows the dump
suffix appended
valid JSON
