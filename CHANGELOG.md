# 0.1.0

First release of the Vanta fork of [BitOne/php-meminfo](https://github.com/BitOne/php-meminfo)
(upstream history up to 1.1.1, 2021). Supports PHP 7.0 – 8.5.

## Added
 - Dumping an **already running process** without changing it: `dump-meminfo <pid>` /
   `meminfo_dump_process()`, served at the next safe point between opcodes (target: PHP 7.1+, NTS,
   POSIX), configured by `meminfo.dump_signal` (default `SIGURG`) and `meminfo.dump_dir`
 - **Browser UI** (`ui/`) that analyzes a dump entirely client-side: leak suspects (retained size),
   growth against an earlier dump, summary, search, reference paths and a graph view
 - Locals of **suspended fibers and Swoole coroutines** (PHP 8.1+), closure and generator state,
   SPL containers and other `get_gc()` content, function `static` variables, registered callbacks
   and user constants
 - Installation with PIE (`pie install vanta/meminfo`) and the `vanta/dump-meminfo` Composer package
   (`vendor/bin/dump-meminfo`)
 - Version and copyright year as build arguments; Docker build/test targets, Swoole and memory
   probes under `tools/repro/`

## Changed
 - Dumps are dozens of times faster (1M objects: from about 35 s to under 1 s), allocate nothing
   from the Zend heap, no longer leave memory behind in the dumped process and no longer crash on
   deeply nested structures
 - Failures throw (`InvalidArgumentException`, `RuntimeException`) instead of writing a truncated
   dump; `dump-meminfo` maps them onto exit codes 2 and 1
 - `meminfo.c` is split into modules with internal symbols hidden

## Fixed
 - Dumps are always valid JSON (invalid UTF-8, NUL in keys, objects without properties such as
   ext-protobuf messages)
 - Crashes and corrupt dumps under Swoole and with suspended fibers on PHP 8.1, loading on PHP 7.4,
   and `__debugInfo` being called on PHP 7.0 – 7.3
 - Variables attributed to the wrong function, `<GLOBAL>` labels on coroutine frames
 - `make` on GNU Make 3.81 (macOS)

## Removed
 - The PHP CLI analyzer (`analyzer/`, `bin/analyzer`): the browser UI replaces it
