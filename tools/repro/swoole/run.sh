#!/bin/sh
# Every Swoole scenario, run inside the meminfo-swoole image (make repro-swoole).
# Prints PASS/FAIL/INFO lines and exits 1 if anything expected is missing.

DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/swoole-out}
MOCK="-d swoole.enable_fiber_mock=1"
failed=0

mkdir -p "$OUT"
php --ri swoole | grep -E '^Version'
php -v | head -1

check() {
    php -n "$DIR/check.php" "$@" || failed=1
}

# Without the fiber mock no extension can see suspended coroutines: only the
# dumper's own chain, globals and statics are expected.
echo "== inside a coroutine, no fiber mock"
OUT="$OUT" php "$DIR/inside.php" || failed=1
check inside "$OUT/inside.json" MarkerGlobal MarkerDumperLocal \
    --gap MarkerSleepLocal MarkerChannelLocal MarkerRunLocal MarkerNestedLocal
check after "$OUT/after.json" MarkerGlobal

for hook in "" 1; do
    echo "== inside a coroutine, fiber mock${hook:+, SWOOLE_HOOK_ALL}"
    HOOK_ALL=$hook OUT="$OUT" php $MOCK "$DIR/inside.php" || failed=1
    # The parent of the dumping coroutine is suspended inside go(): Swoole
    # reports that switch with a frame of its own, so it stays out of reach.
    check inside "$OUT/inside.json" MarkerGlobal MarkerDumperLocal MarkerSleepLocal \
        MarkerChannelLocal MarkerRunLocal --gap MarkerNestedLocal
    ticks=$(cat "$OUT/inside.ticks")
    if [ "$ticks" = 0 ]; then
        echo "PASS inside: no other coroutine ran during the dump"
    else
        echo "FAIL inside: other coroutines ran $ticks time(s) during the dump"
        failed=1
    fi
done

echo "== 10000 suspended coroutines, fiber mock"
OUT="$OUT" N=10000 php -d memory_limit=1G $MOCK "$DIR/many.php" || failed=1
check many "$OUT/many.json" MarkerCoroutineLocal

for idle in "" 1; do
    echo "== dump-meminfo on a coroutine loop${idle:+ (idle)}, fiber mock"
    rm -f "$OUT"/meminfo-*.json
    PIDFILE="$OUT/loop.pid" IDLE=$idle php $MOCK "$DIR/loop.php" > "$OUT/loop.log" 2>&1 &
    sleep 1
    pid=$(cat "$OUT/loop.pid")
    if file=$(dump-meminfo "$pid" --dir="$OUT" --timeout=5); then
        if [ -n "$idle" ]; then
            echo "INFO loop: an idle loop served the dump"
        fi
        check loop "$file" MarkerGlobal MarkerSuspended
    elif [ -n "$idle" ]; then
        echo "INFO loop: an idle loop runs no PHP code, so the dump did not start (expected)"
    else
        echo "FAIL loop: dump-meminfo failed"
        failed=1
    fi
    if kill -0 "$pid" 2>/dev/null; then
        echo "PASS loop: target still alive"
        kill "$pid"
    else
        echo "FAIL loop: target died"
        cat "$OUT/loop.log"
        failed=1
    fi
done

[ $failed = 0 ] && echo "ALL PASSED" || echo "SOME CHECKS FAILED"
exit $failed
