--TEST--
Check that suspended Fibers are dumped once each, from the main script and from inside another Fiber
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (PHP_VERSION_ID < 80100) die('skip Fibers need PHP 8.1');
?>
--FILE--
<?php
    class FirstLocal {}
    class SecondLocal {}
    class MainLocal {}

    function markers($dump)
    {
        $found = array();
        foreach ($dump['items'] as $item) {
            if (isset($item['class']) && in_array($item['class'], array('FirstLocal', 'SecondLocal', 'MainLocal'), true)) {
                $found[] = $item['class'] . ' ' . ($item['is_root'] ? $item['symbol_name'] . '@' . $item['frame'] : 'child');
            }
        }
        sort($found);
        return implode("\n", $found) . "\n";
    }

    function dumpNow()
    {
        $stream = fopen('php://memory', 'rw');
        meminfo_dump($stream);
        rewind($stream);
        return json_decode(stream_get_contents($stream), true);
    }

    function main()
    {
        $mainLocal = new MainLocal();

        $first = new Fiber(function () {
            $firstLocal = new FirstLocal();
            Fiber::suspend();
        });
        $first->start();

        // Dumps from inside: the first fiber and main() are both suspended.
        $second = new Fiber(function () {
            $secondLocal = new SecondLocal();
            Fiber::suspend(dumpNow());
        });

        echo "-- from inside a fiber\n";
        echo markers($second->start());

        echo "-- from main()\n";
        echo markers(dumpNow());

        $first->resume();
        $second->resume();
    }

    main();
?>
--EXPECTF--
-- from inside a fiber
FirstLocal firstLocal@{closure%S}()
MainLocal mainLocal@main()
SecondLocal secondLocal@{closure%S}()
-- from main()
FirstLocal firstLocal@{closure%S}()
MainLocal mainLocal@main()
SecondLocal secondLocal@{closure%S}()
