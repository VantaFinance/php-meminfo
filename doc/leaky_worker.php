<?php
/**
 * A worker that leaks the way real ones do, to try every part of the dump and the UI on a
 * dump of any size: listeners registered per job and never removed, a cache without a limit,
 * duplicate strings, an array emptied in place that keeps its slots, cycles nobody can
 * reach (with gc disabled), an enum and a suspended fiber. Needs PHP 8.1+.
 *
 * It marks a generation per job and stops leaking after MAX_JOBS jobs (default: never),
 * then waits for a dump between jobs. About 0.9 MB of dump per job: 225 jobs ≈ 200 MB.
 *
 *   MAX_JOBS=225 php -d extension=meminfo.so -d memory_limit=-1 \
 *       -d meminfo.dump_defer=1 -d meminfo.track_allocations=1 \
 *       -d meminfo.dump_unreachable=1 -d meminfo.dump_string_hashes=1 \
 *       doc/leaky_worker.php &
 *   dump-meminfo $! --dir=/tmp
 *
 * The worker writes the dump at its next quiet point, prints its path to stderr and exits.
 */
gc_disable();

enum Status: string { case Pending = 'pending'; case Done = 'done'; }

final class Order
{
    public array $lines = [];
    public function __construct(public int $id, public string $customer, public Status $status) {}
}

final class Dispatcher
{
    public static array $listeners = [];
    public static function listen(string $event, Closure $listener): void { self::$listeners[$event][] = $listener; }
}

final class ResponseCache
{
    private static array $entries = [];
    public static function put(string $key, array $value): void { self::$entries[$key] = $value; }
}

final class Node { public ?Node $other = null; public string $payload; }

$queue = [];
for ($i = 0; $i < 200000; $i++) { $queue[] = $i; }
$queue = array_slice($queue, 0, 10); // a copy keeps 10; refill below then empty in place
$sparse = range(1, 100000);
for ($i = 10; $i < 100000; $i++) { unset($sparse[$i]); }

$fiber = new Fiber(function () { $held = new Order(-1, 'fiber', Status::Pending); Fiber::suspend(); return $held; });
$fiber->start();

$customers = ['ACME Corporation International Ltd.', 'Globex Corporation of North America', 'Initech Software Solutions'];
$job = 0;
// Stops leaking after this many jobs (env MAX_JOBS) and keeps serving: the size of the dump.
$maxJobs = (int) (getenv('MAX_JOBS') ?: 100000);
$deadline = microtime(true) + 600;
while (microtime(true) < $deadline) {
    if ($job >= $maxJobs) {
        $path = meminfo_dump_if_requested();
        if ($path !== null) { fwrite(STDERR, "dumped $path after $job jobs\n"); exit(0); }
        usleep(1000);
        continue;
    }
    $job++;
    meminfo_mark();
    for ($n = 0; $n < 200; $n++) {
        $order = new Order($job * 1000 + $n, str_repeat($customers[$n % 3], 2), $n % 2 ? Status::Done : Status::Pending);
        for ($l = 0; $l < 4; $l++) { $order->lines[] = ['sku' => 'SKU-' . ($n * 4 + $l), 'qty' => $l + 1, 'note' => str_repeat('fragile; ', 4)]; }
        // Leak 1: a listener per order, never removed.
        Dispatcher::listen('order.shipped', function () use ($order) { return $order->id; });
        // Leak 2: an unbounded cache.
        ResponseCache::put('order:' . $order->id, ['order' => $order, 'rendered' => str_repeat('<li>line</li>', 8)]);
    }
    if ($job % 50 === 0) {
        // Leak 3: cycles nobody can reach, with gc disabled.
        $a = new Node(); $b = new Node(); $a->other = $b; $b->other = $a; $a->payload = str_repeat('x', 256); unset($a, $b);
    }
    $path = meminfo_dump_if_requested();
    if ($path !== null) { fwrite(STDERR, "dumped $path\n"); exit(0); }
    usleep(1000);
}
