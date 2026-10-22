--TEST--
Check that meminfo_dump() inside a Swoole coroutine sees globals and never lets another coroutine run mid-dump
--SKIPIF--
<?php
    if (!extension_loaded('json')) die('skip json ext not loaded');
    if (!extension_loaded('swoole')) die('skip ext-swoole not loaded');
?>
--FILE--
<?php
    class MarkerGlobal {}
    class MarkerDumper {}

    $global = new MarkerGlobal();
    $bulk = range(1, 50000);
    $ticks = array();
    $path = __DIR__ . '/dump-swoole_coroutine.json.tmp';

    Swoole\Coroutine::set(array('hook_flags' => SWOOLE_HOOK_ALL));

    Swoole\Coroutine\run(function () use (&$ticks, $path) {
        $stop = false;

        Swoole\Coroutine::create(function () use (&$ticks, &$stop) {
            while (!$stop) {
                $ticks[] = hrtime(true);
                $GLOBALS['churn'] = range(1, 100);
                Swoole\Coroutine::sleep(0.001);
            }
        });

        Swoole\Coroutine::create(function () use (&$ticks, &$stop, $path) {
            $dumper = new MarkerDumper();
            Swoole\Coroutine::sleep(0.01);

            // A hooked plain file: fwrite() would yield.
            $stream = fopen($path, 'w');
            $started = hrtime(true);
            meminfo_dump($stream);
            $ended = hrtime(true);

            $ran = 0;
            foreach ($ticks as $tick) {
                $ran += $tick > $started && $tick < $ended ? 1 : 0;
            }
            echo "coroutines run during the dump: ", $ran, "\n";
            fclose($stream);
            $stop = true;
        });
    });

    $meminfoData = json_decode(file_get_contents($path), true);
    unlink($path);

    $roots = array();
    foreach ($meminfoData['items'] as $item) {
        if (!empty($item['is_root']) && isset($item['class']) && strncmp($item['class'], 'Marker', 6) === 0) {
            $roots[$item['class']] = $item['symbol_name'];
        }
    }
    ksort($roots);
    var_dump($roots);
?>
--EXPECT--
coroutines run during the dump: 0
array(2) {
  ["MarkerDumper"]=>
  string(6) "dumper"
  ["MarkerGlobal"]=>
  string(6) "global"
}
