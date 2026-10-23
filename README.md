MEMINFO
=======

![Build](https://github.com/VantaFinance/php-meminfo/workflows/Build%20and%20test%20the%20extension/badge.svg)

PHP Meminfo is a PHP extension that writes the content of PHP memory to a JSON file: every array,
object and value still reachable, the variable or static it hangs from, and what refers to what. Its
main goal is to help you find memory leaks in long-running PHP processes. The dump is analyzed in a
[browser UI](https://vantafinance.github.io/php-meminfo/) that runs entirely in your browser and
points at what keeps memory alive and what grows. A process can be dumped from its own code with
`meminfo_dump()`, or from outside, while it runs, with `dump-meminfo <pid>`.

One of the main sources of inspiration for this tool is the Java jmap tool with the `-histo` option.

Compatibility
-------------

Minimum supported version: **PHP 7.0**. Supported: PHP 7.0 – 8.5 (tested in CI on every version).
Dumping another, running process needs the target to be PHP 7.1 or later, non-ZTS, on POSIX.

Installation
------------

### Quick start with Make

A `Makefile` in the repository root wraps the build. Run `make help` to list every target.

```bash
$ make build          # build the extension (phpize + configure + make, as needed)
$ make install-ext    # install it into the PHP extension directory (may need sudo)
$ make install-cli    # install the dump-meminfo command into $(PREFIX)/bin, default /usr/local
$ make test           # run the extension's .phpt suite
```

To build against a specific PHP installation, override the toolchain variables:

```bash
$ make build PHPIZE=/usr/local/php-7.4/bin/phpize PHP_CONFIG=/usr/local/php-7.4/bin/php-config
```

The reported version and copyright year are build arguments (`make build MEMINFO_VERSION=2.1.0`); see
[Development](doc/development.md#version-and-copyright-year).

### Compilation

From the `extension` directory:

```bash
$ phpize
$ ./configure --enable-meminfo
$ make
$ make install
```

### Docker

In your own image:

```dockerfile
RUN git clone https://github.com/VantaFinance/php-meminfo && cd php-meminfo/extension && phpize && ./configure --enable-meminfo && make && make install && echo 'extension=meminfo.so' >> /usr/local/etc/php/conf.d/docker-php-ext-meminfo.ini
```

`make docker-build PHP_VERSION=8.1` builds a ready image (`meminfo:php-8.1`) from the repository
`Dockerfile`; see [Development](doc/development.md) for the Docker test matrix and the IDE setup.

### Enabling the extension

Add the following line to your `php.ini`:

```ini
extension=meminfo.so
```

### Settings

Both settings are `PHP_INI_SYSTEM` and only matter for [dumping a running process](#dumping-a-running-process):

| Setting | Default | Meaning |
|---|---|---|
| `meminfo.dump_signal` | `SIGURG` | Signal the process dumps on, by name or number. `0` or empty disables it. |
| `meminfo.dump_dir` | system temp dir | Where dumps are written unless the trigger passes `--dir`. |

`php -i | grep meminfo` shows what a build resolved. Details, and why `SIGURG`, are in
[Dumping a running process](doc/process_dump.md#settings).

Usage
-----

### Dumping memory content

```php
meminfo_dump(fopen('/tmp/my_dump_file.json', 'w'));
```

This writes a dump of the PHP memory, as JSON, to the given stream: a file, or `php://stdout`. It
throws `RuntimeException` when the stream cannot be written to, or when the dump is incomplete. [`doc/example.php`](doc/example.php)
is a minimal script (`make dump-example` runs it).

### Dumping a running process

When a worker is already running and leaking, ask it for a dump from outside, without changing or
restarting it. The target only needs the extension loaded; it writes the dump at the next safe point
between two opcodes, and the command prints its path:

```bash
$ dump-meminfo <pid> [--dir=DIR] [--signal=SIG] [--timeout=SEC] [--force]
$ dump-meminfo 12345 --dir=/tmp
/tmp/meminfo-1834201933.json
```

The same from PHP: `$path = meminfo_dump_process(12345, ['dir' => '/tmp']);`. Every failure throws;
the command exits with 1 on a runtime failure and 2 on bad usage. Under php-fpm, only a worker that is
serving a request can be dumped — take its pid from the status page, never the master's.

Errors, settings, FPM, containers, security and limitations are covered in
[Dumping a running process](doc/process_dump.md).

### Analyzing a dump in the browser

Open <https://vantafinance.github.io/php-meminfo/> and drop the dump on the page. The page is static:
the dump is read and analyzed in your browser and never leaves it. To run it locally (Node 24+):

```bash
$ make ui-install
$ make ui-dev         # then open the printed URL
```

It shows the leak suspects (what retains the most memory), growth against an earlier dump of the same
process, a summary by type and class, the largest collections, a search, and for any item every
reference path from a root and a graph of what keeps it reachable. See `ui/README.md` for building
and testing it.

[Hunting down memory leaks](doc/hunting_down_memory_leaks.md) walks through a real investigation with
it, step by step.

Other memory debugging tools for PHP
------------------------------------

- [Xdebug](https://xdebug.org/): its trace feature with memory deltas shows memory usage per function.
- [PHP Memprof](https://github.com/arnaud-lb/php-memory-profiler): aggregated memory usage by
  function, far less resource intensive than a full Xdebug trace.

FAQ
---

### `memory_usage` is much higher than what the dump accounts for

A lot of memory is used by the Zend Engine itself (compiled code, the virtual machine, the garbage
collector) and by extensions. PDO and mysqli, for example, buffer whole result sets in PHP memory by
default ([buffering](https://www.php.net/manual/en/mysqlinfo.concepts.buffering.php)), and image
extensions hold large buffers. All of it goes through the Zend Memory Manager, so it counts towards
`memory_get_usage()` and `memory_limit`, but meminfo only sees the data structures of your program,
not the extensions' own memory. Hence the gap, which can be large.

### "Call to undefined function" when calling `meminfo_dump`

The extension is not enabled. Look for the meminfo section in `php -i` (or a `phpinfo()` page) and
check the `extension=meminfo.so` line of the `php.ini` that PHP actually loads (`php --ini`).

### Why are most tests "skipped"?

The tests need JSON support, but `make test` runs them in a clean environment that loads no shared
extensions. If JSON is built as a separate extension (common before PHP 8), they are skipped. Run them
with your own configuration from the `extension` directory:

```bash
$ TEST_PHP_EXECUTABLE=$(which php) $(which php) run-tests.php -d extension=$PWD/modules/meminfo.so
```

More on building and testing in [Development](doc/development.md).

Credits
-------

Thanks to Derick Rethans for his inspirational work on the essential Xdebug. See https://xdebug.org/
