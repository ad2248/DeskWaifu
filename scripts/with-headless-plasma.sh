#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} != --session ]]; then
    session_dir=$(mktemp -d /tmp/deskwaifu-plasma.XXXXXXXX)
    trap 'if [[ $(realpath -- "$session_dir") == "$session_dir" && $session_dir == /tmp/deskwaifu-plasma.* ]]; then rm -rf -- "$session_dir"; fi' EXIT
    export XDG_RUNTIME_DIR="$session_dir/runtime"
    export XDG_CONFIG_HOME="$session_dir/config" XDG_CACHE_HOME="$session_dir/cache"
    export XDG_DATA_HOME="$session_dir/data" XDG_STATE_HOME="$session_dir/state"
    install -dm700 "$XDG_RUNTIME_DIR"
    unset DISPLAY WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS
    export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE KDE_FULL_SESSION=true
    export QT_QPA_PLATFORM=wayland LIBGL_ALWAYS_SOFTWARE=1
    dbus-run-session -- bash "$0" --session "$@"
    exit
fi
shift

kwin_pid= plasma_pid=
cleanup() {
    local status=$?
    for pid in "$plasma_pid" "$kwin_pid"; do
        if [[ -n $pid ]]; then kill "$pid" 2>/dev/null || true; fi
    done
    wait || true
    if ((status != 0)); then
        for log in kwin plasma; do
            if [[ -f $XDG_RUNTIME_DIR/$log.log ]]; then
                printf '\n%s startup log\n' "$log" >&2
                tee < "$XDG_RUNTIME_DIR/$log.log" >&2
            fi
        done
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for() {
    local pid=$1 description=$2
    shift 2
    local deadline=$((SECONDS + 30))
    while kill -0 "$pid" 2>/dev/null; do
        if "$@"; then return 0; fi
        if ((SECONDS >= deadline)); then break; fi
        sleep 0.1
    done
    printf 'Timed out or process exited while waiting for %s\n' "$description" >&2
    return 1
}

QT_FORCE_STDERR_LOGGING=1 kwin_wayland --virtual --width 1280 --height 800 --socket wayland-ci --no-lockscreen \
    > "$XDG_RUNTIME_DIR/kwin.log" 2>&1 &
kwin_pid=$!
export WAYLAND_DISPLAY=wayland-ci
wait_for "$kwin_pid" 'KWin Wayland socket' test -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
dbus-update-activation-environment WAYLAND_DISPLAY

plasma_ready() {
    busctl --user --auto-start=no --timeout=1 call \
        org.kde.plasmashell /PlasmaShell org.freedesktop.DBus.Peer Ping >/dev/null 2>&1
}
plasmashell --no-respawn > "$XDG_RUNTIME_DIR/plasma.log" 2>&1 &
plasma_pid=$!
wait_for "$plasma_pid" 'Plasma Shell' plasma_ready
busctl --user --auto-start=no --timeout=2 call org.kde.KWin /KWin org.freedesktop.DBus.Peer Ping
printf 'KWin and Plasma Shell ready on %s (1280x800)\n' "$WAYLAND_DISPLAY"

if (($#)); then "$@"; fi
kill -0 "$kwin_pid" "$plasma_pid"
