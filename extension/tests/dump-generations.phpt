--TEST--
meminfo_mark() starts object generations: an object's generation is the mark that first saw it
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class Marked {}
    class Reused {}

    function generations()
    {
        $dump = fopen('php://memory', 'rw');
        meminfo_dump($dump);
        rewind($dump);
        $data = json_decode(stream_get_contents($dump), true);
        fclose($dump);

        $found = array();
        foreach ($data['items'] as $item) {
            if (isset($item['symbol_name']) && $item['frame'] === '<GLOBAL>' && isset($item['class'])) {
                $found[$item['symbol_name']] = isset($item['generation']) ? $item['generation'] : null;
            }
        }
        ksort($found);

        return json_encode($found);
    }

    $old = new Marked();
    echo "before any mark: ", generations(), "\n";

    echo "mark: ", meminfo_mark(), "\n";
    $new = new Marked();
    echo "after the first mark: ", generations(), "\n";

    $temporary = new Marked();
    echo "mark: ", meminfo_mark(), "\n";
    // spl_object_id() exists from 7.2; before, the reuse is not checked.
    $handle = function_exists('spl_object_id') ? spl_object_id($temporary) : null;
    unset($temporary);
    // Takes the freed handle: another object, a generation of its own.
    $reused = new Reused();
    echo "handle reused: ", var_export($handle === null || spl_object_id($reused) === $handle, true), "\n";
    echo "after the second mark: ", generations(), "\n";
?>
--EXPECT--
before any mark: {"old":null}
mark: 1
after the first mark: {"new":2,"old":1}
mark: 2
handle reused: true
after the second mark: {"new":2,"old":1,"reused":3}
