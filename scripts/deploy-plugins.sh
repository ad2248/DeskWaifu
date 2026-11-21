#!/usr/bin/env bash
# Stage already-built plugins next to the development host; never invokes Cargo.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
usage='Usage: scripts/deploy-plugins.sh [--release] (set CARGO_TARGET_DIR for a custom target directory)'
profile=debug
case "$#:${1:-}" in
    0:) ;;
    1:--release) profile=release ;;
    1:--help|1:-h) echo "$usage"; exit 0 ;;
    *) echo "$usage" >&2; exit 2 ;;
esac

target_dir=${CARGO_TARGET_DIR:-$root/target}
output=$target_dir/$profile
plugins=(libdeskwaifu_test_plugin.so libdeskwaifu_voice_input.so libdeskwaifu_terminal.so)
for artifact in deskwaifu "${plugins[@]}"; do
    if [[ ! -f "$output/$artifact" ]]; then
        printf 'Missing build artifact: %s\nBuild the workspace with Cargo before deploying plugins.\n' "$output/$artifact" >&2
        exit 1
    fi
done
install -Dm755 -t "$output/plugins" "${plugins[@]/#/$output/}"
