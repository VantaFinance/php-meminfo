<?php
/**
 * meminfo_dump() from inside a coroutine while siblings are suspended, then
 * from the main script once Co\run() returned.
 *
 *   OUT=/tmp/out [HOOK_ALL=1] php [-d swoole.enable_fiber_mock=1] inside.php
 *
 * Writes inside.json and after.json, and reports how many times another
 * coroutine ran during the dump: with the file hook (SWOOLE_HOOK_ALL, which
 * Co\run() turns on by default since Swoole 5) a plain fwrite() yields, and
 * anything that runs then can free what the dump is walking.
 */

use Swoole\Coroutine as Co;
use Swoole\Coroutine\Channel;

class MarkerGlobal {}
class MarkerDumperLocal {}
class MarkerNestedLocal {}
class MarkerSleepLocal {}
class MarkerChannelLocal {}
class MarkerRunLocal {}

$global = new MarkerGlobal();
// Big enough for the dump to write its buffer out many times mid-traversal.
$bulk = range(1, 50000);
$out = getenv('OUT') ?: '/tmp';
$ticks = array();

if (getenv('HOOK_ALL')) {
    Co::set(array('hook_flags' => SWOOLE_HOOK_ALL));
}

Co\run(function () use ($out, &$ticks) {
    $runLocal = new MarkerRunLocal();
    $release = new Channel(1);
    $done = new Channel(4);
    $stop = false;

    go(function () use ($done) {
        $sleepLocal = new MarkerSleepLocal();
        $big = range(1, 100000);
        Co::sleep(0.5);
        $done->push(count($big));
    });

    go(function () use ($release, $done) {
        $channelLocal = new MarkerChannelLocal();
        $big = array_fill(0, 50000, 'x');
        $done->push($release->pop() + count($big));
    });

    // Mutates a global while it runs, and notes when it got to.
    go(function () use (&$ticks, &$stop, $done) {
        while (!$stop) {
            $ticks[] = hrtime(true);
            $GLOBALS['churn'] = range(1, 100);
            Co::sleep(0.001);
        }
        $done->push(1);
    });

    go(function () use ($release, $done, $out, &$ticks, &$stop) {
        $nestedLocal = new MarkerNestedLocal();

        go(function () use ($release, $out, &$ticks, &$stop) {
            $dumperLocal = new MarkerDumperLocal();
            // Opened first: with the file hook, fopen() itself may yield.
            $stream = fopen("$out/inside.json", 'w');
            $started = hrtime(true);
            meminfo_dump($stream);
            $ended = hrtime(true);

            $ran = 0;
            foreach ($ticks as $tick) {
                if ($tick > $started && $tick < $ended) {
                    $ran++;
                }
            }
            fprintf(STDERR, "     inside: dump took %.3fs, other coroutines ran %d time(s) during it\n",
                ($ended - $started) / 1e9, $ran);
            file_put_contents("$out/inside.ticks", (string) $ran);
            $stop = true;
            $release->push(1);
        });

        $done->push(1);
    });

    for ($i = 0; $i < 4; $i++) {
        $done->pop();
    }
    $runLocal = null;
});

meminfo_dump(fopen("$out/after.json", 'w'));
