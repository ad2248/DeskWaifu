#!/usr/bin/env bash
# Build the host, independent CLI and Rust plugins together, then stage plugins.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
profile=debug
native=false
kwin=false
args=()
usage='Usage: scripts/build.sh [--release] [--native-live2d] [--kwin-cursor]'
while (( $# )); do
    case "$1" in
        --release) profile=release ;;
        --native-live2d) native=true ;;
        --kwin-cursor) native=true; kwin=true ;;
        --help|-h) echo "$usage"; exit 0 ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
    shift
done
cd "$root"
target_dir=${CARGO_TARGET_DIR:-target}
if [[ $profile == release ]]; then
    args+=(--release)
fi
if $native; then
    target_dir=${CARGO_TARGET_DIR:-target/live2d-native}
    if $kwin; then
        args+=(--features deskwaifu-terminal/kwin-cursor)
    else
        args+=(--features deskwaifu-terminal/native)
    fi
fi
cargo build --workspace --locked --target-dir "$target_dir" "${args[@]}"
for plugin in libdeskwaifu_test_plugin.so libdeskwaifu_voice_input.so; do
    install -Dm755 "$target_dir/$profile/$plugin" "$target_dir/$profile/plugins/$plugin"
done
if $native; then
    install -Dm755 "$target_dir/$profile/libdeskwaifu_terminal.so" "$target_dir/$profile/plugins/libdeskwaifu_terminal.so"
fi
