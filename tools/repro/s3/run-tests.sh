#!/usr/bin/env bash
# Run .phpt tests of an S3-enabled image against a throwaway S3-compatible
# server (versitygw, posix backend) on a private Docker network.
#
#   run-tests.sh <image> [tests...]        (default: tests/)
#
# Environment: DOCKER (docker), S3_GATEWAY_IMAGE (pinned versitygw),
# DOCKER_PLATFORM_FLAG, S3_RUN (the command run in the image instead of the
# tests, e.g. "bash" for a shell next to the server), S3_EXTRA_ENV.
set -euo pipefail

IMAGE="$1"; shift
TESTS="${*:-tests/}"
DOCKER="${DOCKER:-docker}"
GATEWAY_IMAGE="${S3_GATEWAY_IMAGE:-versity/versitygw:v1.10.0}"
NAME="meminfo-s3-$$"
ACCESS_KEY="meminfo-test"
SECRET_KEY="meminfo-test-secret"

cleanup() {
    "$DOCKER" rm -f "$NAME-gw" >/dev/null 2>&1 || true
    "$DOCKER" network rm "$NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$DOCKER" network create "$NAME" >/dev/null
"$DOCKER" run -d --name "$NAME-gw" --network "$NAME" --network-alias s3 \
    -e ROOT_ACCESS_KEY_ID="$ACCESS_KEY" -e ROOT_SECRET_ACCESS_KEY="$SECRET_KEY" \
    --entrypoint sh "$GATEWAY_IMAGE" -c 'mkdir -p /data && exec versitygw --port :7070 posix /data' >/dev/null

echo "s3-test: $GATEWAY_IMAGE up as http://s3:7070 on network $NAME" >&2

ENV_FLAGS=(
    -e MEMINFO_TEST_S3_ENDPOINT=http://s3:7070
    -e AWS_ACCESS_KEY_ID="$ACCESS_KEY"
    -e AWS_SECRET_ACCESS_KEY="$SECRET_KEY"
    -e AWS_REGION=us-east-1
)
# Extra NAME=value pairs for the test container, separated by spaces
# (make s3-test-asan passes the sanitizer runtime this way).
for pair in ${S3_EXTRA_ENV:-}; do
    ENV_FLAGS+=(-e "$pair")
done

if [ -n "${S3_RUN:-}" ]; then
    tty_flags=()
    if [ -t 0 ]; then
        tty_flags=(-it)
    fi
    # shellcheck disable=SC2086
    "$DOCKER" run --rm ${tty_flags[@]+"${tty_flags[@]}"} ${DOCKER_PLATFORM_FLAG:-} --network "$NAME" "${ENV_FLAGS[@]}" "$IMAGE" sh -c "$S3_RUN"
    exit
fi

# shellcheck disable=SC2086
"$DOCKER" run --rm ${DOCKER_PLATFORM_FLAG:-} --network "$NAME" "${ENV_FLAGS[@]}" "$IMAGE" \
    sh -c 'for i in $(seq 1 50); do php -r "exit(@fsockopen(\"s3\", 7070) ? 0 : 1);" && break; sleep 0.2; done;
           make test-one TEST="$0"' "$TESTS"
