#!/usr/bin/env bash
# Build the host, independent CLI and Rust plugins together, then stage plugins.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
profile=debug
args=()
case "${1:-}" in
    '') ;;
    --release) profile=release; args+=(--release); shift ;;
    --help|-h) echo 'Usage: scripts/build.sh [--release]'; exit 0 ;;
    *) echo 'Usage: scripts/build.sh [--release]' >&2; exit 2 ;;
esac
if (( $# )); then
    echo 'Usage: scripts/build.sh [--release]' >&2
    exit 2
fi
cd "$root"
target_dir=${CARGO_TARGET_DIR:-target}
cargo build --workspace --locked --target-dir "$target_dir" "${args[@]}"
install -Dm755 "$target_dir/$profile/libdeskwaifu_test_plugin.so" \
    "$target_dir/$profile/plugins/libdeskwaifu_test_plugin.so"
