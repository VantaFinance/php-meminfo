<?php
/**
 * N suspended coroutines, each holding an object of its own; dumped from
 * inside a coroutine.
 *
 *   OUT=/tmp/out N=10000 php [-d swoole.enable_fiber_mock=1] many.php
 */

use Swoole\Coroutine as Co;
use Swoole\Coroutine\Channel;

class MarkerCoroutineLocal
{
    public $payload;

    public function __construct($i)
    {
        $this->payload = str_repeat('x', 64) . $i;
    }
}

$n = (int) (getenv('N') ?: 10000);
$out = getenv('OUT') ?: '/tmp';

Co\run(function () use ($n, $out) {
    $gate = new Channel($n);

    for ($i = 0; $i < $n; $i++) {
        go(function () use ($i, $gate) {
            $local = new MarkerCoroutineLocal($i);
            $gate->pop();
        });
    }

    $started = microtime(true);
    meminfo_dump(fopen("$out/many.json", 'w'));
    fprintf(STDERR, "     many: %d coroutines, dump took %.3fs, %d bytes\n",
        Co::stats()['coroutine_num'], microtime(true) - $started, filesize("$out/many.json"));

    for ($i = 0; $i < $n; $i++) {
        $gate->push(1);
    }
});
