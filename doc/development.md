Development
===========

Notes for working on the extension itself. The root `Makefile` wraps everything; `make help` lists
every target. The browser UI has its own `ui-*` targets, described in [`ui/README.md`](../ui/README.md).

Building and testing
--------------------

```bash
$ make build          # phpize + configure + make, as needed
$ make rebuild        # force a clean rebuild
$ make test           # the .phpt suite (alias of test-ext)
$ make test-one TEST=tests/dump-array.phpt   # a single test, path relative to extension/
$ make lint           # php -l over the repository's PHP scripts
$ make check          # lint + the .phpt suite
$ make ci             # what CI runs: build + the .phpt suite
$ make clean          # remove all build artifacts and dependencies
```

To build against a specific PHP installation, override the toolchain variables:

```bash
$ make build PHPIZE=/usr/local/php-7.4/bin/phpize PHP_CONFIG=/usr/local/php-7.4/bin/php-config PHP=/usr/local/php-7.4/bin/php
```

The same without Make, from the `extension` directory:

```bash
$ phpize
$ ./configure --enable-meminfo
$ make
$ REPORT_EXIT_STATUS=1 NO_INTERACTION=1 make test
```

Every change must keep the PHP 7.0 – 8.5 CI matrix green; engine API differences are handled with
`PHP_VERSION_ID` preprocessor guards.

Building and testing in Docker
------------------------------

To work on the extension without a local PHP toolchain, the repository `Dockerfile` builds it on top
of `php:<version>-cli-alpine`. The PHP version is selected with `PHP_VERSION` (`7.0` … `8.5`, default
`8.1`):

```bash
$ make docker-build PHP_VERSION=7.4   # build the image meminfo:php-7.4 with the extension enabled
$ make docker-test  PHP_VERSION=7.4   # run the .phpt suite inside that image
$ make docker-matrix                  # docker-test for every version in PHP_VERSIONS
$ make docker-shell PHP_VERSION=7.4   # open a bash shell in the image
$ make docker-clean                   # remove all meminfo:php-* images
```

The extension only lives inside the image; nothing is written to the host. Use the image to take a
dump of your own script:

```bash
$ docker run --rm -v "$PWD":/app meminfo:php-8.1 php /app/script.php
```

Set `DOCKER_PLATFORM` to build for another architecture, e.g. `DOCKER_PLATFORM=linux/amd64` on Apple
Silicon. The first build of each version is slow, especially for PHP 7.0 whose Alpine mirror can take
several minutes to install the compiler toolchain; later builds reuse the cached layer.

`make repro`, `make repro-test` and `make repro-build` do the same with ext-protobuf (and optionally
ext-grpc, `WITH_GRPC=1`) loaded; see `tools/repro/protobuf/`.

IDE setup (CLion)
-----------------

`CMakeLists.txt` exists only to give CLion code insight into the Zend API; the extension is still
built with phpize. The PHP headers come from the Docker image of the version you want to work
against:

```bash
$ make ide PHP_VERSION=8.1   # copy the PHP 8.1 headers into .ide/php-8.1/
$ make ide-clean             # remove all extracted headers
```

Then in CLion:

1. Right-click `CMakeLists.txt` → **Load CMake Project**.
2. In Settings → Build, Execution, Deployment → CMake, disable the auto-created `Debug` profile and
   enable the `php-X.Y` profile(s) from `CMakePresets.json`.
3. Select the profile matching the extracted headers. `#if PHP_VERSION_ID` branches follow that
   version.

When switching to a version whose headers are missing, run `make ide PHP_VERSION=X.Y` first — CMake
stops with that hint otherwise.
