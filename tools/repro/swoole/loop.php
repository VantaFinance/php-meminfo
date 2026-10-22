<?php
/**
 * A long-running coroutine loop to dump from outside with dump-meminfo.
 *
 *   PIDFILE=/tmp/loop.pid [IDLE=1] php [-d swoole.enable_fiber_mock=1] loop.php
 *
 * IDLE=1 leaves out the ticker: every coroutine then waits in Co::sleep(), no
 * PHP code runs, and a signal dump cannot start until something wakes it.
 */

use Swoole\Coroutine as Co;

class MarkerGlobal {}
class MarkerSuspended {}
class MarkerTicker {}

$global = new MarkerGlobal();
file_put_contents(getenv('PIDFILE') ?: '/tmp/loop.pid', (string) getmypid());

Co\run(function () {
    go(function () {
        $suspended = new MarkerSuspended();
        $big = range(1, 200000);
        Co::sleep(1000);
    });

    if (!getenv('IDLE')) {
        go(function () {
            $ticker = new MarkerTicker();
            while (true) {
                Co::sleep(0.05);
            }
        });
    }

    Co::sleep(1000);
});
