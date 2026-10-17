--TEST--
meminfo_dump() inside a Swoole coroutine sees globals and the locals of suspended coroutines (fiber mock)
--SKIPIF--
<?php
if (!extension_loaded('json')) die('skip json ext not loaded');
if (!extension_loaded('swoole')) die('skip ext-swoole not loaded');
if (PHP_VERSION_ID < 80100) die('skip fiber observers need PHP 8.1');
?>
--INI--
swoole.enable_fiber_mock=1
--FILE--
<?php
class MarkerGlobal {}
class MarkerSuspended {}
class MarkerDumper {}
$global = new MarkerGlobal();

Swoole\Coroutine\run(function () {
    $chan = new Swoole\Coroutine\Channel(1);
    Swoole\Coroutine::create(function () use ($chan) {
        $suspended = new MarkerSuspended();
        $chan->pop();
    });
    Swoole\Coroutine::create(function () use ($chan) {
        $dumper = new MarkerDumper();
        $stream = fopen('php://memory', 'w+');
        meminfo_dump($stream);
        rewind($stream);
        $dump = json_decode(stream_get_contents($stream), true);
        $roots = [];
        foreach ($dump['items'] as $item) {
            if (!empty($item['is_root']) && strncmp($item['class'] ?? '', 'Marker', 6) === 0) {
                $roots[$item['class']] = $item['symbol_name'];
            }
        }
        ksort($roots);
        var_dump(json_last_error() === JSON_ERROR_NONE, $roots);
        $chan->push(1);
    });
});
?>
--EXPECT--
bool(true)
array(3) {
  ["MarkerDumper"]=>
  string(6) "dumper"
  ["MarkerGlobal"]=>
  string(6) "global"
  ["MarkerSuspended"]=>
  string(9) "suspended"
}
