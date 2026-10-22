Hunting down memory leaks
=========================

This guide walks through finding a memory leak with meminfo and its browser UI: what to dump, when,
and how to read the result until the line of code that leaks is found.

What a leak looks like in PHP
-----------------------------

A PHP request rarely leaks in a way that matters: whatever it allocates is freed when it ends. Leaks
hurt in processes that live long — queue consumers, daemons, schedulers, long-running CLI commands,
event-loop servers, and php-fpm workers with a high `pm.max_requests`. There, memory that should be
freed after each job stays reachable from something that lives as long as the process:

- a static property or a global used as a cache that is never evicted;
- a listener, callback or logger handler registered once per job on a long-lived dispatcher;
- an identity map or unit of work (Doctrine's `EntityManager`, for example) that is never cleared;
- an array that collects "processed" ids, errors or log lines for the life of the process.

The symptoms are memory usage that grows with every job until `memory_limit` is hit, and more and
more time spent in the garbage collector, which keeps scanning objects it cannot free.

meminfo does not track allocations. It writes everything still reachable at the moment of the dump —
every array, object and value, the variable or static it hangs from, and what refers to what. A leak
is then something that is reachable but should not be, and the question becomes *what keeps it*.

Taking the dumps
----------------

Take **two dumps of the same process**: a baseline once it has warmed up, and a later one after many
more jobs. Whatever grew between the two is what leaks; whatever is merely big but stable is not.

From the code, at a point you choose (between two jobs, not in the middle of one):

```php
meminfo_dump(fopen('/tmp/worker-' . getmypid() . '-' . $jobs . '.json', 'w'));
```

From outside, without changing or restarting the process, with `dump-meminfo` (the target needs the
extension loaded, PHP 7.1+, non-ZTS, on POSIX):

```bash
$ dump-meminfo 12345 --dir=/tmp       # baseline
/tmp/meminfo-1834201933.json
$ dump-meminfo 12345 --dir=/tmp       # some minutes or jobs later
/tmp/meminfo-2911470521.json
```

A php-fpm worker can only be dumped while it is serving a request; take its pid from the FPM status
page, never the master's. Everything about the signal, the settings, FPM, containers and what to do
when the command refuses is in [Dumping a running process](process_dump.md).

A dump contains everything the process had in memory — keys, strings, tokens. Treat it as sensitive.

Opening a dump
--------------

Open <https://vantafinance.github.io/php-meminfo/> and drop the dump on the page (or use
**Open dump…**). The page is static: the file is read and analyzed by your browser, in a Web Worker,
and nothing is uploaded.

To run the UI yourself (Node 24+):

```bash
$ make ui-install
$ make ui-dev        # open the printed URL
```

The left column lists the views: **Leak suspects**, **Growth**, **Summary**, **Top children**,
**Search** and **Graph**. Selecting any item in a table opens the inspector on the right.

A worked example
----------------

[`leak_example.php`](leak_example.php) is a small worker with a classic leak. It handles orders in a
loop; for each one it registers an `AuditListener` on a long-lived `Dispatcher`, and never removes it.
Each listener holds its `Order`, so no order is ever freed:

```php
function handle_order(Dispatcher $dispatcher, $id)
{
    $order = new Order($id);

    // The leak: a listener per order, registered on a long-lived dispatcher
    // and never removed. It holds the order, so no order is ever freed.
    $dispatcher->listen('order.shipped', new AuditListener($order));
}
```

It dumps after 100 orders and again after 1000:

```bash
$ php doc/leak_example.php /tmp
/tmp/leak-before.json
/tmp/leak-after.json
```

Open `/tmp/leak-after.json`.

### 1. Leak suspects: what keeps the most alive

**Leak suspects** ranks items by their *retained size*: how much memory would be freed if that item
went away, everything it alone keeps alive included. It is computed from the dominator tree of the
dump — item A dominates item B when every path from a root to B goes through A.

- **Biggest retainers** lists the items that dominate the most: here `Dispatcher dispatcher` in
  `<GLOBAL>`, holding over 99% of the dump, with about 19,000 items in **Items kept**. Long chains
  are folded into their head, so a linked list shows up once, not once per node.
- **Classes retaining the most** sums the same per class. `AuditListener` and `Order` both show
  1,000 **Instances**, all of them **Top-most** (not kept by another instance of the same class).
  Selecting a class row lists its instances in Search.
- **Roots by frame** shows which frames (`<GLOBAL>`, a function, a class's statics) hold the most.

Data reachable from several roots is dominated by none of them; it is flagged **shared** and listed
on its own rather than charged to any one root.

A big retainer is a lead, not a verdict: a large configuration array or a container is supposed to
be big. What matters is whether it grows.

### 2. Growth: compare with the baseline

In **Growth**, click **Choose baseline dump…** and pick `/tmp/leak-before.json`. Only the
baseline's totals per type and class are kept, so this does not need memory for a second dump.

The table lists, for each type or class, the count **Before**, **Now**, **Δ count** and
**Δ self size**; **Only types that changed** hides the rest. Here `AuditListener` and `Order` grew by
900 each — exactly the 900 orders handled between the two dumps — and arrays, strings and ints grew
by a multiple of that (the order lines). A count that grows in step with the work done is the
signature of a leak. Classes marked **new** appeared since the baseline, **gone** ones disappeared.
Selecting a row searches for its instances.

### 3. Summary and Top children

**Summary** counts every item by type, objects by class, with **Count** and **Self size** and their
shares. On its own it answers "what is in memory"; it is most useful to check that a class you
expected to be gone (a request, an entity, a response) is still there in numbers.

**Top children** lists the arrays and objects with the most **Children**. A growing collection is the
usual shape of a leak, and here the top row is an array with 1,000 children retaining almost the
whole dump: the dispatcher's `order.shipped` listener list. Clicking its **Address** opens it in the
graph; selecting the row opens the inspector.

### 4. Search

**Search** finds items by field: `class`, `type`, `symbol`, `frame`, `is root`, `size`,
`object handle`, `address`. Every filter must match (**Add filter** for more); **is** compares
exactly and **matches** takes a regular expression. Searching `class` **is** `Order` returns 1,000
items. **Go to address** jumps straight to an item when you have its address from somewhere else.

Pick any `Order` from the results.

### 5. Inspector: why is it still here

The inspector shows the item's **Self size**, **Retained**, **Items kept alive**, **Distance from a
root**, **Children** and **Referenced by**, then:

- **Kept alive by** — the dominator chain from a root down to the item: each entry alone keeps the
  next one in memory. For our order it reads `Dispatcher dispatcher` → `array` → `array` →
  `AuditListener` → `Order`.
- **Reference paths from roots** — the actual paths, with the key or property at each step:

  ```
  Dispatcher dispatcher → [listeners] array → [order.shipped] array → [0] AuditListener → [order]
  ```

  This is the answer: the order is reachable because the dispatcher's `listeners['order.shipped']`
  still holds an `AuditListener`, whose `order` property holds it.
- **Children** and **Referenced by** — what the item holds and who holds it. Selecting a row moves
  the inspector there; **← Back** returns.

### 6. Graph: what to cut

Click **Show graph** in the inspector (or an address anywhere). In **Paths from roots** mode the graph
merges every shortest path from a root to the item, roots on the left and the item on the right.
Items that *every* path goes through are highlighted ("every path goes through it" in the legend):
free any one of them and the item goes. Items shared by several roots have their own colour.

With one root, every step is such a bottleneck; in real dumps there are usually several roots and
only a few highlighted items — those are where the fix goes. **Neighbourhood** draws the items around
one instead (**Depth** 1–3, **Follow** children, parents or both); double-click a node to bring in its
neighbours, click it to inspect it, **Fit** to re-centre, **Show as list** to go back to the
inspector. A graph is capped at 600 items, nearest roots first, and says so when it is cut short.

### 7. Fix and confirm

The listener list is the place to cut: the listener should be registered once, or removed when the
order is done. For example:

```php
$listener = new AuditListener($order);
$dispatcher->listen('order.shipped', $listener);
// ... handle the order ...
$dispatcher->forget('order.shipped', $listener);
```

Then take the two dumps again and load them as before. **Growth** should no longer show
`AuditListener` or `Order` growing between the baseline and the later dump, and **Leak suspects**
should no longer show the dispatcher retaining most of the dump. If something else now grows, that is
the next leak — repeat from step 1.

Pitfalls
--------

- **Retained sizes are estimates.** Item sizes are the extension's estimates of the zvals and
  structures it sees, not allocator truth; compare them with each other, not with `memory_usage`.
  An object first reached from a function's local may also appear without its later holders (a
  global or a static that also holds it emits no item of its own), so its retainer can be misplaced.
- **Memory outside PHP values is not in the dump.** Compiled code, the engine's own structures and
  buffers held by extensions (unfetched PDO/mysqli result sets, images in GD/Imagick) count towards
  `memory_get_usage()` but are not items. A large gap between **in use** in the top bar and what the
  dump accounts for points there.
- **Unreachable cycles are not in the dump.** Objects that only refer to each other are garbage
  waiting for the cycle collector; meminfo walks from the roots and never sees them. If memory drops
  after `gc_collect_cycles()`, that was the cause, not a leak.
- **Idle processes do not serve signal dumps.** `dump-meminfo` is answered at the next opcode. An
  idle php-fpm worker has no handler installed, and a worker or event loop blocked in `sleep()`,
  `stream_select()` or another long internal call is woken by the signal but only dumps once it gets
  back to PHP code. Ask while it is busy, or dump from the code.
- **Swoole: turn on `swoole.enable_fiber_mock=1`** (PHP 8.1+) in the workers you dump. Without it the
  locals of suspended coroutines — often where a per-request leak sits — are not in the dump, while
  `memory_usage` still counts them. See [Dumping a running process](process_dump.md#limitations).
- **Very large dumps need browser memory.** The UI streams the file into compact typed arrays, but a
  dump of tens of millions of items can still exceed what a tab may hold (Chrome manages about
  2.5 GB of dump). The UI warns before loading a file that large and says so if a tab ran out of
  memory. Dump earlier, or dump a smaller process (fewer jobs, a smaller batch) — a leak shows in
  Growth long before it fills the memory limit.
