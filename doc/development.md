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

Version and copyright year
--------------------------

The version `phpversion('meminfo')` and `phpinfo()` report, and the last year of the copyright
notice, are build arguments:

| configure option | Make variable | Docker build arg | Default |
|---|---|---|---|
| `--with-meminfo-version=VER` | `MEMINFO_VERSION` | `MEMINFO_VERSION` | content of `extension/VERSION` |
| `--with-meminfo-copyright-year=YYYY` | `MEMINFO_COPYRIGHT_YEAR` | `MEMINFO_COPYRIGHT_YEAR` | year of `SOURCE_DATE_EPOCH` when set, else the current year |

```bash
$ make build MEMINFO_VERSION=2.1.0 MEMINFO_COPYRIGHT_YEAR=2026
$ make docker-build PHP_VERSION=8.3 MEMINFO_VERSION=2.1.0
$ ./configure --with-meminfo-version=2.1.0 --with-meminfo-copyright-year=2026   # without Make
```

The version may contain only letters, digits and `. + ~ -`; configure rejects anything else. `make
build` reconfigures whenever either value changes and prints what the build uses. CI builds a tag as
its name (without a leading `v`), any other commit as `<VERSION>+g<short sha>`, with the year of the
commit date, so the same commit always builds the same strings. A build with no arguments at all
(`phpize && ./configure`, PECL) uses the defaults.

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

The S3 transport (`--with-meminfo-s3`) has its own image, built on the Debian `php:<version>-cli`
image with aws-c-s3 from vcpkg, and a local S3 server ([versitygw](https://github.com/versity/versitygw))
started next to it:

```bash
$ make s3-test          PHP_VERSION=8.4   # the .phpt suite, dump-s3_*.phpt included
$ make s3-test-valgrind PHP_VERSION=8.4   # the S3 tests with the dumping processes under valgrind
$ make repro-s3-fpm     PHP_VERSION=8.4   # PHP-FPM scenarios: per-pool settings, hung uploads, clear_env
$ make s3-shell         PHP_VERSION=8.4   # a shell in the image, the S3 server at http://s3:7070
```

`S3_TESTS` narrows `s3-test` to some tests. To build with S3 outside Docker, `make vcpkg-install`
installs the dependencies into `vcpkg_installed/` and `make build MEMINFO_S3=vcpkg_installed/<triplet>`
links them in.

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
