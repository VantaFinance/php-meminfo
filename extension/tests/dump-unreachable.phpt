--TEST--
Objects alive but reached from no root (an uncollected cycle) are dumped only with the "unreachable" option
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
?>
--FILE--
<?php
    class CycleMember
    {
        public $other;
    }

    gc_disable();
    $a = new CycleMember();
    $b = new CycleMember();
    $a->other = $b;
    $b->other = $a;
    unset($a, $b);

    function dump_with(array $options)
    {
        $dump = fopen('php://memory', 'rw');
        meminfo_dump($dump, $options);
        rewind($dump);
        $data = json_decode(stream_get_contents($dump), true);
        fclose($dump);

        $members = array();
        foreach ($data['items'] as $item) {
            if (isset($item['class']) && $item['class'] === 'CycleMember') {
                $members[] = $item['is_root'] ? $item['frame'] . ' ' . preg_replace('/\d+/', 'N', $item['symbol_name']) : 'child';
            }
        }
        sort($members);

        return implode(', ', $members) ?: 'none';
    }

    echo "default: ", dump_with(array()), "\n";
    echo "unreachable: ", dump_with(array('unreachable' => true)), "\n";
    echo "unreachable off: ", dump_with(array('unreachable' => false)), "\n";

    gc_enable();
    echo "collected: ", gc_collect_cycles(), "\n";
    echo "after collection: ", dump_with(array('unreachable' => true)), "\n";
?>
--EXPECT--
default: none
unreachable: <UNREACHABLE> #N, child
unreachable off: none
collected: 2
after collection: none
