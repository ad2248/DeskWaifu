#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/quit-exit-status.sh"

BUILD_DIR=${1:-build-ci}
DEPS_DIR=${DESKWAIFU_DEPS_DIR:-}
RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/deskwaifu-runtime}
LOG_DIR="$BUILD_DIR/test-logs"
SWAY_PID=
APP_PID=
CONFIG_DIR=

cleanup() {
    if [[ -n ${APP_PID} ]]; then
        kill "$APP_PID" 2>/dev/null || true
        wait "$APP_PID" 2>/dev/null || true
    fi
    if [[ -n ${SWAY_PID} ]]; then
        kill "$SWAY_PID" 2>/dev/null || true
        wait "$SWAY_PID" 2>/dev/null || true
    fi
    if [[ -n ${CONFIG_DIR} ]]; then
        rm -rf "$CONFIG_DIR"
    fi
}
trap cleanup EXIT

if [[ -z $DEPS_DIR ]]; then
    DEPS_DIR=$(bash "$(dirname "$0")/find-deps-dir.sh") || {
        echo "Set DESKWAIFU_DEPS_DIR to a complete dependency directory" >&2
        exit 1
    }
fi
if [[ ! -d "$DEPS_DIR/CubismSdkForNative-5-r.5/Samples/Resources" ||
      ! -d "$DEPS_DIR/glew" ]]; then
    echo "DESKWAIFU_DEPS_DIR must contain CubismSdkForNative-5-r.5 and glew" >&2
    exit 1
fi

mkdir -p "$LOG_DIR"
install -d -m 0700 "$RUNTIME_DIR"
rm -f "$RUNTIME_DIR"/wayland-* "$RUNTIME_DIR/deskwaifu.sock"

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
CONFIG_DIR=$(mktemp -d "$RUNTIME_DIR/hotkey-config.XXXXXX")
export XDG_CONFIG_HOME="$CONFIG_DIR"
python3 tests/hotkey_smoke.py prepare "$DEPS_DIR/CubismSdkForNative-5-r.5/Samples/Resources/Haru"
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
for _ in {1..100}; do
    for socket in "$RUNTIME_DIR"/sway-ipc.*.sock; do
        if [[ -S $socket ]]; then export SWAYSOCK=$socket; break 2; fi
    done
    sleep 0.1
done
if [[ -z ${SWAYSOCK:-} ]]; then echo 'Sway IPC socket missing' >&2; exit 1; fi

"$BUILD_DIR/bin/deskwaifu" \
    --models_dir "$XDG_CONFIG_HOME/deskwaifu/models" \
    >"$LOG_DIR/deskwaifu.log" 2>&1 &
APP_PID=$!

for _ in {1..200}; do
    if [[ -S $RUNTIME_DIR/deskwaifu.sock ]]; then
        break
    fi
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "DeskWaifu exited before creating its IPC socket" >&2
        cat "$LOG_DIR/deskwaifu.log" >&2
        exit 1
    fi
    sleep 0.1
done

if [[ ! -S $RUNTIME_DIR/deskwaifu.sock ]]; then
    echo "Timed out waiting for the DeskWaifu IPC socket" >&2
    cat "$LOG_DIR/deskwaifu.log" >&2
    exit 1
fi

for _ in {1..100}; do
    response=$(timeout 2 ./deskwaifu-ctl get_status 2>/dev/null || true)
    if [[ $response == *'"ok":true'* || $response == *'"ok": true'* ]]; then
        break
    fi
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "DeskWaifu exited before IPC became responsive" >&2
        cat "$LOG_DIR/deskwaifu.log" >&2
        exit 1
    fi
    sleep 0.1
done

if [[ ${response:-} != *'"ok":true'* && ${response:-} != *'"ok": true'* ]]; then
    echo "Timed out waiting for a successful IPC response" >&2
    cat "$LOG_DIR/deskwaifu.log" >&2
    exit 1
fi

assert_ok() {
    local response
    response=$(./deskwaifu-ctl "$@")
    if [[ $response != *'"ok":true'* && $response != *'"ok": true'* ]]; then
        echo "IPC command failed: $*" >&2
        echo "$response" >&2
        exit 1
    fi
}

assert_ok get_status
assert_ok get_available_models
assert_ok get_current_model
assert_ok get_expressions
assert_ok set_expression --id F01
assert_ok get_motions
assert_ok do_motion --group Idle --index 0 --priority 2
assert_ok set_mouth_y --value 0.5
assert_ok set_mouth_y --reset
./deskwaifu-ctl get_control_status | python3 -c 'import json,sys; s=json.load(sys.stdin); assert s["ok"] and not s["has_mouth"], s'
assert_ok set_mouth_y --value 0.5
assert_ok set_model_zoom --value 1.25
assert_ok set_model_position --x 0.1 --y -0.1
assert_ok get_model_position
assert_ok set_look --x 0.25 --y -0.25
assert_ok set_look --reset
assert_ok release_source
./deskwaifu-ctl get_control_status | python3 -c 'import json,sys; s=json.load(sys.stdin); assert s["ok"] and not s["has_mouth"] and not s["has_look"], s'
assert_ok set_mouth_y --value 0.5
assert_ok get_control_status
python3 - <<'PY'
import json
import os
import socket

path = os.path.join(os.environ['XDG_RUNTIME_DIR'], 'deskwaifu.sock')

def connect():
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(10)
    client.connect(path)
    return client

def request(client, command, **kwargs):
    client.sendall((json.dumps(dict(command=command, **kwargs)) + '\n').encode())
    return json.loads(client.makefile('rb').readline())

with connect() as observer:
    assert request(observer, 'get_control_status')['mouth'] == 0.5
with connect() as first, connect() as second:
    assert request(first, 'set_mouth_y', value=0.8, stream_source=True)['ok']
    assert request(second, 'set_look', x=0.25, y=-0.25, stream_source=True)['ok']
    with connect() as observer:
        state = request(observer, 'get_control_status')
        assert state['mouth'] == 0.8 and state['look_x'] == 0.25, state
    first.close()
    with connect() as observer:
        # Disconnection is processed on the next frame; use a bounded retry.
        import time
        for _ in range(100):
            state = request(observer, 'get_control_status')
            if state['mouth'] == 0.5: break
            time.sleep(0.01)
        assert state['mouth'] == 0.5 and state['has_look'], state
with connect() as observer:
    import time
    for _ in range(100):
        state = request(observer, 'get_control_status')
        if not state['has_look']: break
        time.sleep(0.01)
    assert state['mouth'] == 0.5 and not state['has_look'], state
PY
assert_ok toggle_hidden
python3 - <<'PY'
import json
import os
import socket

def request(command, **values):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        connection.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'deskwaifu.sock'))
        connection.sendall((json.dumps({'command': command, **values}) + '\n').encode())
        return json.loads(connection.makefile('rb').readline())

assert request('get_status')['hidden'] is True
initial = request('get_model_position')
for command in ({'command': 'set_model_zoom', 'value': 'nan'},
                {'command': 'set_model_position', 'x': 'inf'},
                {'command': 'do_motion', 'group': 'missing', 'index': 0},
                {'command': 'set_model', 'index': 999}):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        connection.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'deskwaifu.sock'))
        connection.sendall((json.dumps(command) + '\n').encode())
        response = json.loads(connection.makefile('rb').readline())
        assert not response['ok'], (command, response)
assert request('get_model_position') == initial
assert request('toggle_hidden')['hidden'] is False
PY
assert_ok switch_skin
assert_ok next_model
python3 - <<'PY'
import json
import os
import socket
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
    client.settimeout(10)
    client.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'deskwaifu.sock'))
    client.sendall(b'{"command":"get_control_status"}\n')
    state = json.loads(client.makefile('rb').readline())
    assert not state['has_mouth'] and not state['has_look'], state
PY
assert_ok prev_model
assert_ok set_model --name Haru
python3 tests/hotkey_smoke.py check

kill -0 "$APP_PID"
timeout 20 "$BUILD_DIR/bin/deskwaifu-control" --smoke-test \
    >"$LOG_DIR/deskwaifu-control.log" 2>&1
kill -0 "$APP_PID"
{
    # Exercise the actual layer-surface configure/EGL rebind path, including
    # switching back with an already live GPU context and active model.
    swaymsg create_output >/dev/null
    for _ in {1..100}; do
        count=$(swaymsg -t get_outputs -r | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
        if (( count >= 2 )); then break; fi
        sleep 0.1
    done
    if (( count < 2 )); then echo 'Second headless output missing' >&2; exit 1; fi
    original=$(swaymsg -t get_outputs -r | python3 -c 'import json,sys; print(next(o["name"] for o in json.load(sys.stdin) if o["focused"]))')
    target=$(swaymsg -t get_outputs -r | python3 -c 'import json,sys; a=json.load(sys.stdin); print(next(o["name"] for o in a if not o["focused"]))')
    before=$(grep -c 'Output switch configured and committed:' "$LOG_DIR/deskwaifu.log" || true)
    swaymsg focus output "$target" >/dev/null
    kill -USR2 "$APP_PID"
    for _ in {1..100}; do
        after=$(grep -c 'Output switch configured and committed:' "$LOG_DIR/deskwaifu.log" || true)
        if (( after > before )); then break; fi
        sleep 0.1
    done
    if (( after <= before )); then cat "$LOG_DIR/deskwaifu.log" >&2; exit 1; fi
    assert_ok get_status
    swaymsg focus output "$original" >/dev/null
    kill -USR2 "$APP_PID"
    for _ in {1..100}; do
        after=$(grep -c 'Output switch configured and committed:' "$LOG_DIR/deskwaifu.log" || true)
        if (( after > before + 1 )); then break; fi
        sleep 0.1
    done
    if (( after <= before + 1 )); then cat "$LOG_DIR/deskwaifu.log" >&2; exit 1; fi
    assert_ok get_status
}
assert_ok quit
for _ in {1..200}; do
    state=$(ps -o stat= -p "$APP_PID" 2>/dev/null || true)
    if [[ -z $state || $state == Z* ]]; then break; fi
    sleep 0.1
done
state=$(ps -o stat= -p "$APP_PID" 2>/dev/null || true)
if [[ -n $state && $state != Z* ]]; then
    echo "DeskWaifu did not exit after quit" >&2
    exit 1
fi
if wait "$APP_PID"; then
    APP_PID=
else
    exit_code=$?
    APP_PID=
    report_app_failure "$exit_code" "$LOG_DIR/deskwaifu.log" || exit "$exit_code"
fi
grep -q 'GPU resources released before window teardown' "$LOG_DIR/deskwaifu.log"
echo "Headless Wayland, Qt control, and IPC smoke test passed"
