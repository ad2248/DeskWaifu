#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:-build-ci}
DEPS_DIR=${WAIFULAND_DEPS_DIR:-/opt/waifuland-deps}
RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/waifuland-runtime}
LOG_DIR="$BUILD_DIR/test-logs"
SWAY_PID=
APP_PID=

cleanup() {
    if [[ -n ${APP_PID} ]]; then
        kill "$APP_PID" 2>/dev/null || true
        wait "$APP_PID" 2>/dev/null || true
    fi
    if [[ -n ${SWAY_PID} ]]; then
        kill "$SWAY_PID" 2>/dev/null || true
        wait "$SWAY_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

mkdir -p "$LOG_DIR"
install -d -m 0700 "$RUNTIME_DIR"
rm -f "$RUNTIME_DIR"/wayland-* "$RUNTIME_DIR/waifuland.sock"

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe

sway --config containers/sway.config >"$LOG_DIR/sway.log" 2>&1 &
SWAY_PID=$!

for _ in {1..100}; do
    for socket in "$RUNTIME_DIR"/wayland-*; do
        if [[ -S $socket ]]; then
            export WAYLAND_DISPLAY=${socket##*/}
            break 2
        fi
    done
    if ! kill -0 "$SWAY_PID" 2>/dev/null; then
        echo "Sway exited before creating a Wayland socket" >&2
        cat "$LOG_DIR/sway.log" >&2
        exit 1
    fi
    sleep 0.1
done

if [[ -z ${WAYLAND_DISPLAY:-} ]]; then
    echo "Timed out waiting for the Wayland socket" >&2
    cat "$LOG_DIR/sway.log" >&2
    exit 1
fi
wayland-info >/dev/null

"$BUILD_DIR/bin/waifuland" \
    --models_dir "$DEPS_DIR/CubismSdkForNative-5-r.5/Samples/Resources" \
    >"$LOG_DIR/waifuland.log" 2>&1 &
APP_PID=$!

for _ in {1..200}; do
    if [[ -S $RUNTIME_DIR/waifuland.sock ]]; then
        break
    fi
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "Waifuland exited before creating its IPC socket" >&2
        cat "$LOG_DIR/waifuland.log" >&2
        exit 1
    fi
    sleep 0.1
done

if [[ ! -S $RUNTIME_DIR/waifuland.sock ]]; then
    echo "Timed out waiting for the Waifuland IPC socket" >&2
    cat "$LOG_DIR/waifuland.log" >&2
    exit 1
fi

for _ in {1..100}; do
    response=$(timeout 2 ./waifuland-ctl get_status 2>/dev/null || true)
    if [[ $response == *'"ok":true'* || $response == *'"ok": true'* ]]; then
        break
    fi
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "Waifuland exited before IPC became responsive" >&2
        cat "$LOG_DIR/waifuland.log" >&2
        exit 1
    fi
    sleep 0.1
done

if [[ ${response:-} != *'"ok":true'* && ${response:-} != *'"ok": true'* ]]; then
    echo "Timed out waiting for a successful IPC response" >&2
    cat "$LOG_DIR/waifuland.log" >&2
    exit 1
fi

assert_ok() {
    local response
    response=$(./waifuland-ctl "$@")
    if [[ $response != *'"ok":true'* && $response != *'"ok": true'* ]]; then
        echo "IPC command failed: $*" >&2
        echo "$response" >&2
        exit 1
    fi
}

assert_ok get_status
assert_ok get_available_models
assert_ok set_model_zoom --value 1.25
assert_ok set_model_position --x 0.1 --y -0.1
assert_ok toggle_hidden

kill -0 "$APP_PID"
echo "Headless Wayland and IPC smoke test passed"
