Dumping a running process
=========================

Calling `meminfo_dump()` requires changing the application. When a worker is already running and
leaking, a dump can instead be requested from outside, with the `dump-meminfo` command or
`meminfo_dump_process()` from PHP. This page is the full reference; the [README](../README.md#dumping-a-running-process)
has the short version.

How it works
------------

```bash
$ dump-meminfo <pid> [--dir=DIR] [--signal=SIG] [--timeout=SEC] [--force] [--count=N] [--interval=TIME]
$ dump-meminfo 12345
/tmp/meminfo-1834201933.json
```

| Option | Meaning |
|---|---|
| `--dir=DIR` | Directory to write the dump to (default: the target's own `meminfo.dump_dir`) |
| `--signal=SIG` | Signal to send, by name or number (default: this process's `meminfo.dump_signal`, normally `SIGURG`) |
| `--timeout=SEC` | Seconds to wait for the dump to start (default: 30); once started, the wait continues while it grows |
| `--force` | Send the signal even when the target does not appear to handle it |
| `--count=N` | Take N dumps in a row and print one path per line, in order (default: 1) |
| `--interval=TIME` | Wait between two dumps of a series: seconds, or a number with `s` or `m` (default: `60s`) |

The only requirement on the target is that the extension is loaded. On receiving the signal it
records the request and raises the engine's interrupt flag; the dump itself is written at the next
safe point between two opcodes, so the application is never interrupted mid-operation.

On systems without `sigqueue()`, such as macOS, the request cannot carry an id, so the target names
the file after its pid and the time the dump started (`meminfo-12345-1792181893-19785.json`) and the
trigger looks for a file of that shape written after it asked.

Without `--dir` the target writes to its own `meminfo.dump_dir`. With it, the dump is written to
`DIR` (relative paths are resolved by the caller): the trigger leaves the directory in a request
file, `/tmp/meminfo-request-<pid>…`, which the target reads when it serves the signal and the trigger
removes afterwards. If the target cannot write there, the trigger fails at once with the target's
reason instead of waiting out `--timeout`. A target running a meminfo that predates this cannot be
told where to write; look in its `meminfo.dump_dir`.

The command prints the path of the finished dump on stdout and nothing else, so it composes with
other commands (`cp "$(dump-meminfo 12345)" ~/dumps/`). Open the dump in the
[browser UI](hunting_down_memory_leaks.md#opening-a-dump).

The same thing is available from PHP; it returns the path of the finished dump:

```php
$path = meminfo_dump_process(12345, ['dir' => '/var/tmp', 'timeout' => 60]);
```

Install the command with `make install-cli` (honours `PREFIX`, default `/usr/local`).

Errors
------

Nothing is reported by a return value — every failure throws, so a dump cannot be mistaken for a
success that happens to have produced no file:

```php
try {
    $path = meminfo_dump_process($pid);
} catch (InvalidArgumentException $e) {
    // The call itself was wrong: a pid that cannot be signalled, an unknown
    // signal name, no signal configured.
} catch (RuntimeException $e) {
    // The target or the environment: not handling the signal, no longer
    // running, timed out, a result that is not the file it should be.
}
```

`meminfo_dump()` throws `RuntimeException` as well, when the dump cannot be written to the stream it
was given, or when it is incomplete because the extension ran out of memory for its own bookkeeping
(the stream then holds valid JSON with items missing). Note that the stream holds a partial dump
either way: the bytes written before the failure are not rolled back, unlike the signal path, which
writes to a temporary file and removes it.

`dump-meminfo` maps the same distinction onto its exit code:

| Exit | Meaning |
|---|---|
| 0 | The dump was written. Its path is on stdout, and nothing else is. |
| 1 | A runtime failure — the target or the environment. |
| 2 | Bad usage, from either the command's own argument parsing or the call. |

The message always goes to stderr, so `$(dump-meminfo PID)` stays usable.

Settings
--------

| Setting | Default | Meaning |
|---|---|---|
| `meminfo.dump_signal` | `SIGURG` | Signal the process dumps on, by name (with or without the `SIG` prefix) or number. `0` or empty disables it. |
| `meminfo.dump_dir` | system temp dir | Where dump files are written unless the trigger passes `--dir` / `'dir'`. Without it, the trigger looks in its own `meminfo.dump_dir`, so both sides must agree. On macOS the system temp dir (`$TMPDIR`) differs per user — set this explicitly there, or pass `--dir`. |

Both are `PHP_INI_SYSTEM`, so they belong in `php.ini`. The other settings are described below:
[quiet points](#dumping-at-a-quiet-point-of-a-worker), [memory pressure](#dumping-on-memory-pressure),
and the [README](../README.md#settings) for dump options, generations and allocation sites.

```ini
extension=meminfo.so
meminfo.dump_signal=SIGURG
meminfo.dump_dir=/var/tmp/meminfo
```

`php -i | grep meminfo` reports the resolved signal, the directory, and whether this build can dump
on a signal at all.

### Why `SIGURG` is the default

It is the only common signal whose default action is to *ignore* it, so sending it to a process that
happens not to be listening does nothing. `SIGUSR1` and `SIGUSR2` look like the natural choice but are
a poor one: php-fpm resets them to `SIG_DFL` in every worker, where their default action is to
**terminate**, so a mistargeted signal would kill an idle worker. They remain available through
`meminfo.dump_signal` for anyone who needs them.

Finding an FPM worker
---------------------

Enable the status page in the pool configuration:

```ini
pm.status_path = /status
```

Then pick the worker that is actually serving the request you care about — `?full&json` lists one
entry per worker with its `pid`, `state` and `request uri`:

```bash
$ curl -s 'http://127.0.0.1/status?full&json' | jq -r \
    '.processes[] | select(.state == "Running") | "\(.pid) \(."request uri")"'
```

**Aim at a worker, never at the FPM master.** The master interprets `SIGUSR1` as "reopen logs" and
`SIGUSR2` as "reload", and it reports both as handled, so the safety check below cannot refuse them —
sending `SIGUSR2` to the master restarts the whole pool. Note that `pgrep -f 'php-fpm: pool www'`
matches the master as well; take the pid from the status page.

When it refuses
---------------

Before signalling anything, the trigger checks `/proc/<pid>/status` (on macOS, the kernel's process
record) to see whether the target actually handles the signal. The handler exists only for the
duration of a request, so this single check catches all of: the extension is not loaded, the target
is a build that cannot serve a dump, or the worker is idle.

```
$ dump-meminfo 12345
meminfo_dump_process(): process 12345 does not handle signal 23: ...
$ echo $?
1
```

`--force` skips the check. On a system where the check is impossible (no `/proc`, and not macOS),
only the default `SIGURG` is sent without it.

`--force` does not, however, make the trigger send a signal whose default action would terminate or
stop a target that is known not to handle it — there is no dump to be had that way, only a dead
process.

Limitations
-----------

- **The target must be PHP 7.1 or later, non-ZTS, on POSIX.** The interrupt hook the dump relies on
  does not exist before 7.1, and under ZTS a signal arrives on an arbitrary thread. `dump-meminfo` and
  `meminfo_dump_process()` themselves work from any build, including PHP 7.0 — a process that cannot
  serve a dump can still ask another one for its.
- **Under FPM, only a worker that is handling a request can be dumped.** Between requests there is no
  PHP memory worth dumping and no handler installed.
- **The dump happens at the next opcode.** A target blocked in a long internal call or in I/O only
  reaches that point when the call returns. Asking for a dump interrupts the wait, which is visible to
  the application: `sleep()` returns early, `stream_select()` reports an interrupted system call, and
  `pcntl` dispatches any signals it had pending.
- **An idle event loop never answers.** Swoole, ReactPHP or AMPHP waiting in `epoll_wait()` with
  nothing to do runs no opcode, so the request waits until a timer or a request wakes it, and
  `dump-meminfo` times out otherwise. Ask while it is busy, or keep a cheap timer ticking in workers
  you may want to dump (`Swoole\Timer::tick(1000, function () {})`). In Swoole's PROCESS mode the
  master process never runs PHP code: dump a worker instead.
- **Swoole coroutines need `swoole.enable_fiber_mock=1` (PHP 8.1+).** A dump walks the running
  coroutine and the globals; the locals of coroutines that are suspended (in `Co::sleep()`, on a
  channel, waiting for I/O) live on stacks of Swoole's own and are only reachable when Swoole reports
  its switches to the engine, which this setting enables. The parent of the dumping coroutine, while
  it is still inside `go()`, stays out of reach even then. OpenSwoole does not report its switches,
  and neither does PHP before 8.1, so there suspended coroutines are not in the dump.
- **Registered callbacks are dumped under the frame `<CALLBACK>`.** Autoloaders and `pcntl_signal()`
  handlers are kept in tables that SPL and pcntl keep private, so the dump lists them by calling
  `spl_autoload_functions()` and `pcntl_signal_get_handler()`. These internal functions run no PHP
  code, and they are not called within 256 KB of `memory_limit`. Not dumped: the context of
  `libxml_set_streams_context()` and stream notification callbacks, which have no reader.
- **`max_execution_time` is paused for the duration of the dump**, so dumping a large heap cannot time
  out the request it interrupts. FPM's `request_terminate_timeout` and the web server's
  `fastcgi_read_timeout` keep running, though — a dump big enough to outlast them will lose the worker.
- **FPM's slowlog can swallow the signal.** It uses `ptrace` on the worker to collect a backtrace; a
  signal arriving in that window may be lost. Retry.
- **Without `sigqueue()` (macOS), concurrent requests are not told apart.** Two triggers asking the
  same process at the same moment may be handed the same file, and a second one passing `--dir` while
  the first is still waiting is refused ("another dump request is pending"). Requests made one after
  the other always get separate dumps. The signal is resent at most three times, so a target writing
  somewhere the trigger cannot see produces a bounded number of dumps rather than one per second.

Security
--------

A dump contains array keys, class and property names, string contents and heap addresses —
everything the request had in memory. Treat it as sensitive:

- Dump files are created with mode `0600`, owned by the user the target runs as.
- The temporary file is opened with `O_EXCL | O_NOFOLLOW`, so a symlink or a leftover file at that
  path is never written through or into; the attempt is recorded in the target's error log.
- The returned path is verified to be a regular file with a single link, owned by the user the
  target runs as, before it is handed back. The owner comes from `/proc` or, on macOS, from the
  kernel's process record; where it cannot be determined only files of the calling user are accepted,
  and none when the caller is root. Without `sigqueue()`, files of the expected shape that fail this
  check, or are timestamped before the request or in the future, are skipped rather than returned.
- Requesting a dump needs permission to signal the target: the same user, or root. Reading the result
  across mount namespaces additionally needs `CAP_SYS_PTRACE`.
- A signal whose default action would terminate or stop the target is never sent to a process that is
  known not to handle it, not even with `--force`. `--force` only covers the case where that cannot be
  determined.
- A request file from `--dir` is honoured only if it is a regular file with a single link, not a
  symlink, and owned by the target's own user or by root — the users that may signal it anyway — and
  names an absolute directory. Anything else at that name is ignored. Whoever may ask for a dump can
  thus choose where it goes; nobody else can.
- **`meminfo.dump_dir` must not be writable by anyone you do not trust.** The returned path is
  verified before it is handed back, but anything that can write to that directory can replace the
  file afterwards. The default system temporary directory is shared; prefer a dedicated directory
  owned by the user the target runs as.
- Nothing from the dump path ever reaches the target's own output. Failures go to its error log, so a
  dump cannot corrupt a response or trigger a user error handler.

A series of dumps
-----------------

One dump shows what is in memory; three show what keeps growing. With `--count`, the command takes N
dumps of the same process, `--interval` apart, and prints their paths in order. A failure stops the
series; the dumps already taken stay.

```bash
$ dump-meminfo 12345 --count=3 --interval=5m
```

Open the files together in the browser UI. The newest is the dump; the others become its baselines.
The Growth view then shows two things:

- what grew at every step;
- the objects created between the first two dumps that are still alive in the last: leak candidates,
  grouped by where they are held.

Dumping at a quiet point of a worker
------------------------------------

A signal is served at the next safe point. That is usually in the middle of a request or a job,
so the dump also holds that job's temporaries, which blur a comparison of several dumps. With
`meminfo.dump_defer=1`, the signal only records the request. The dump is written when the worker
calls `meminfo_dump_if_requested()` between two jobs. The function returns the dump's path, or `null`
when nothing is pending. The trigger waits as usual, within its `--timeout`. A request nobody served
before the PHP request ends is logged.

```ini
meminfo.dump_defer = 1
```

| Runtime | Where to call `meminfo_dump_if_requested()` |
|---|---|
| RoadRunner | in the worker loop, after `$worker->respond(...)` |
| FrankenPHP worker mode | after the handler in the `frankenphp_handle_request()` loop, next to `gc_collect_cycles()` |
| Symfony Messenger | a listener on `WorkerRunningEvent` (fired between two messages) |
| Laravel Octane | a listener on `RequestTerminated` (or `TaskTerminated` / `TickTerminated`) |
| Plain CLI loop | at the end of each iteration |

```php
// RoadRunner
while ($request = $worker->waitRequest()) {
    $worker->respond($handler->handle($request));
    meminfo_dump_if_requested();
}
```

Dumping on memory pressure
--------------------------

A leak is often noticed only when the process dies on `memory_limit`. Two settings make the process
dump itself:

| Setting | When the dump is written |
|---|---|
| `meminfo.dump_on_limit=1` | When "Allowed memory size … exhausted" is raised. The dump is written before the fatal error ends the request, with the frames still in place. |
| `meminfo.dump_threshold=512M` (or `80%`) | Once per request, at the next safe point after memory usage passes it. With `meminfo.dump_defer`, at the next `meminfo_dump_if_requested()`. |

Both write to `meminfo.dump_dir`, under the same names and with the same `0600` permissions as signal
dumps, and log the dump's path to the error log. The memory limit is lifted while the dump is
written, as for a signal.

The dump on `memory_limit` does not wait for `meminfo.dump_defer`: the fatal error ends the request
right after it, so there is no quiet point left. It therefore holds the job that was running. Its
header says `"trigger": "memory_limit"`, and the log line says so when `dump_defer` is on. The UI warns
when such a dump, or one taken on a signal or a threshold without `dump_defer`, is part of a
comparison. The threshold has no such limit: with `dump_defer` it waits for the next
`meminfo_dump_if_requested()` (`"trigger": "threshold (deferred)"`).

The threshold is checked on function calls through an observer, which is registered only when the
setting is set. On PHP 8.0 and 8.1 the observer sees only calls to user functions; from 8.2 it sees
internal calls too. Before 8.0 the setting is ignored. `dump_on_limit` works from PHP 7.1, like
signal dumps.

Containers
----------

Signals do not cross PID namespaces, so the trigger has to run in the target's:

```bash
$ docker exec <container> dump-meminfo <pid>
$ kubectl exec <pod> -c <container> -- dump-meminfo <pid>
```

An ephemeral debug container shares the PID namespace but not the filesystem, so it will not see the
dump; run inside the target container instead. With `readOnlyRootFilesystem`, point
`meminfo.dump_dir` at a writable volume (an `emptyDir` is enough) and make sure it is large enough —
a dump is roughly 800 bytes per item.

Cleaning up
-----------

A dump is written to `<name>.json.tmp` and renamed into place only once it is complete, so a `.json`
file is always whole. A target killed mid-dump leaves its `.tmp` behind; those are safe to delete:

```bash
$ find /tmp -maxdepth 1 -name 'meminfo-*.json.tmp' -mtime +1 -delete
```
