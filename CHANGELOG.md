# 0.2.0

## Added
 - **Complete roots.** The dump now also reaches:
   - what frames hold besides their variables: `$this`, the running closure, extra and internal-call
     arguments, live temporaries and pending call arguments;
   - lazy objects (8.4) and the callable and return value of fibers;
   - class constants and enum cases;
   - autoloaders, tick functions, `header_register_callback()`, the session save handler,
     `pcntl_signal()` handlers, the libxml entity loader and the default stream context options;
   - output buffers.
 - **Options and functions.**
   - `meminfo_dump($stream, ['unreachable' => true, 'string_hashes' => true])`;
   - `meminfo_mark()` generations;
   - `meminfo_histogram()`;
   - `meminfo_dump_if_requested()` with `meminfo.dump_defer`;
   - `meminfo.dump_on_limit`, `meminfo.dump_threshold` and `meminfo.track_allocations`;
   - `dump-meminfo --count/--interval`.
 - **S3 dump transport** (`--with-meminfo-s3`, aws-c-s3 from vcpkg via `vcpkg.json`). With
   `meminfo.dump_upload=s3://bucket/prefix`, every dump the process writes on its own is uploaded in
   the background — no request waits for it, an exiting process waits up to
   `meminfo.dump_upload_timeout`. `.uploaded` / `.upload-error` records next to the dump,
   `meminfo.dump_upload_keep_local`, S3-compatible stores (`meminfo.s3_endpoint`,
   `meminfo.s3_path_style`), per-pool targets in PHP-FPM, `dump-meminfo --wait-upload` /
   `meminfo_dump_process(..., ['wait_upload' => true])`.
 - **New dump fields.**
   - Header: process context (PHP version, SAPI, pid, time, `memory_limit`, RSS, GC counters, VM
     stack, compiled classes and functions) and what the dump was taken on (`trigger`).
   - Items: closure origin (`function`, `declared_at`), frame `line`, `resource_type`, array
     `capacity`/`used`.
 - **Browser UI.**
   - Findings, "How held" (retainer clusters and merged paths), ignored holders with recomputation.
   - Series and three-dump leak candidates, generations, allocation sites, histograms.
 - **Headless report** for CI (`ui/report/cli.ts`, `npm run build:report`), and a dump-speed
   benchmark with a CI threshold (`make bench-dump`, `make bench-compare`).

## Changed
 - **One item per allocation** (`identity_model: 2`): a string or array shared by many variables
   is dumped once.
 - **Sizes are the allocations items stand for** (`size_model: 2`): array slots and keys, property
   tables, internal object structs. Scalars count 0.
 - On a 2M-item graph, dumps are 7–20% faster and have 15% fewer items.

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
