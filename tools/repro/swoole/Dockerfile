# Adds ext-swoole on top of the meminfo image to check dumps of coroutine-based
# processes: from inside a coroutine, from outside with dump-meminfo, and with
# thousands of suspended coroutines.
#
# Prefer the Makefile wrappers: make repro-swoole / repro-swoole-test PHP_VERSION=8.3

ARG PHP_VERSION=8.3

FROM meminfo:php-${PHP_VERSION}

# Swoole 6 needs PHP 8.2+; 5.1.8 is the line that still builds on PHP 8.1.
ARG SWOOLE_VERSION=6.2.3

RUN apk add --no-cache linux-headers openssl-dev procps

# Built from the PECL tarball with an explicit configure line: no interactive
# pecl prompts, and none of the optional dependencies (brotli, zstd, curl…).
RUN set -eux; \
    mkdir -p /tmp/swoole; \
    cd /tmp/swoole; \
    curl -fsSL -o swoole.tgz "https://pecl.php.net/get/swoole-${SWOOLE_VERSION}.tgz"; \
    tar xzf swoole.tgz; \
    cd "swoole-${SWOOLE_VERSION}"; \
    phpize; \
    ./configure --disable-brotli --disable-zstd; \
    make -j"$(nproc)"; \
    make install; \
    docker-php-ext-enable swoole; \
    cd /; \
    rm -rf /tmp/swoole; \
    php -m | grep -E '^(swoole|meminfo)$'

COPY tools/repro/swoole/ /src/tools/repro/swoole/

CMD ["sh", "/src/tools/repro/swoole/run.sh"]
