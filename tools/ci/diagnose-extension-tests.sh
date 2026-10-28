#!/usr/bin/env bash
# Failure-only CI diagnostics. Keep dump contents and the environment out of logs.
set -uo pipefail
shopt -s nullglob

output=extension/test-diagnostics
mkdir -p "$output/children"

{
    echo '[FIX:ci] PHP runtime and configuration'
    php -v
    php --ini
    php -r 'printf("binary=%s sapi=%s version=%s zts=%d memory_limit=%s temp_dir=%s\n", PHP_BINARY, PHP_SAPI, PHP_VERSION, PHP_ZTS, ini_get("memory_limit"), sys_get_temp_dir());'
    if [ -f extension/modules/meminfo.so ]; then
        php -n -d "extension=$PWD/extension/modules/meminfo.so" --ri meminfo
    fi
    if [ -f extension/config.h ]; then
        grep -E '^#define (PHP_|MEMINFO_|HAVE_SIGQUEUE)' extension/config.h
    fi
} > "$output/runtime.log" 2>&1

echo '::group::[FIX:ci] Runtime diagnostics'
cat "$output/runtime.log"
echo '::endgroup::'

for file in extension/tests/*.diff extension/tests/*.out extension/tests/*.log; do
    echo "::group::[FIX:ci] $file"
    sed -n '1,200p' "$file"
    echo '::endgroup::'
done

# Replay only the failed OOM scenarios, preserving the original PHPT results.
# Tracing is enabled here rather than during the suite so it cannot change
# successful tests' expected stderr. The exit status also exposes crashes.
for test in dump-bailout_mid_walk dump-on_limit dump-s3_on_limit; do
    if [ ! -f "extension/tests/$test.out" ] || [ ! -f "extension/tests/$test.php" ]; then
        continue
    fi
    replay="$output/replay-$test.log"
    MEMINFO_DEBUG_DUMP=1 php -d "extension=$PWD/extension/modules/meminfo.so" \
        -d max_execution_time=60 "extension/tests/$test.php" > "$replay" 2>&1
    status=$?
    echo "[FIX:ci] Replay $test exit status: $status" >> "$replay"
    echo "::group::[FIX:ci] Replay $test"
    sed -n '1,200p' "$replay"
    echo '::endgroup::'
done

# The tests run children with -n and their own error_log. Those failures are
# otherwise lost when the fixture cleans up; CI opts into keeping the files.
temp_dir=$(php -r 'echo sys_get_temp_dir();') || temp_dir=/tmp
for file in "$temp_dir"/meminfo-child-* "$temp_dir"/meminfo-detached-*; do
    case "$file" in
        *.out|*.err|*.log|*.php)
            [ -f "$file" ] || continue
            cp "$file" "$output/children/"
            case "$file" in
                *.err|*.log)
                    [ -s "$file" ] || continue
                    echo "::group::[FIX:ci] Child $(basename "$file")"
                    sed -n '1,120p' "$file"
                    echo '::endgroup::'
                    ;;
            esac
            ;;
    esac
done

echo "[FIX:ci] Diagnostics saved to $output; full PHPT output is in the CI artifact."
