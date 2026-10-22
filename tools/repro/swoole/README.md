# Swoole repro

Checks what a dump holds when the process runs Swoole coroutines. The image is `meminfo:php-X.Y`
plus ext-swoole built from the PECL tarball (6.2.3, or 5.1.8 on PHP 8.1, the last line that
builds there). PHP 8.1+ only.

```bash
make repro-swoole PHP_VERSION=8.3        # every scenario below, PASS/FAIL per check
make repro-swoole-test PHP_VERSION=8.3   # the .phpt suite with ext-swoole loaded
make repro-swoole PHP_VERSION=8.1        # Swoole 5.1.8
```

| Script | Scenario |
|---|---|
| `inside.php` | `meminfo_dump()` from a coroutine while others wait in `Co::sleep()` and `Channel::pop()`, then from the main script after `Co\run()`. Also counts how often another coroutine ran *during* the dump: with the file hook (on by default in `Co\run()` since Swoole 5), `fwrite()` yields |
| `many.php` | 10 000 suspended coroutines, each holding an object, dumped from inside one of them |
| `loop.php` | a long-running coroutine loop dumped from outside with `dump-meminfo <pid>`; `IDLE=1` leaves out the ticker coroutine |
| `check.php` | validates a dump and counts marker classes: `PASS`/`FAIL` for expected ones, `INFO` for known gaps |
| `run.sh` | runs all of the above inside the image (the image's default command) |

What is expected, and why:

- Suspended coroutines are reachable only with `swoole.enable_fiber_mock=1`: Swoole then reports
  every coroutine switch to the engine's fiber observers, which the extension uses to remember
  where each suspended coroutine stopped. Without it no extension can see their stacks.
- The parent of the dumping coroutine, suspended inside `go()`, stays out of reach even then
  (`INFO … known gap`): Swoole reports that switch with a frame of its own on the C stack.
- An idle loop (every coroutine waiting, no timer due) runs no PHP code, so a `dump-meminfo`
  request waits until something wakes it — a request, a timer — and times out otherwise.
