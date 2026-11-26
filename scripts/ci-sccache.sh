#!/usr/bin/env bash
# Run one CI build with exclusive access to a persistent sccache directory.
set -euo pipefail
cache_dir=$1
shift

if ! command -v sccache >/dev/null || [[ -L "$cache_dir" ]] ||
   ! { [[ -d "$cache_dir" ]] || mkdir -- "$cache_dir"; } ||
   ! { exec {cache_lock}>>"$cache_dir/.lock"; }; then
    echo 'sccache unavailable, building without compiler cache'
    exec "$@"
fi
if ! flock -n "$cache_lock"; then
    echo 'sccache cache busy, building without compiler cache'
    exec {cache_lock}>&-
    exec "$@"
fi

# Keep the lock outside the entries that sccache can evict.
export SCCACHE_DIR="$cache_dir/entries" SCCACHE_CACHE_SIZE=4G
# Every job shares the host loopback under host networking, so each cache dir
# gets its own socket instead of racing for the default TCP port.
export SCCACHE_SERVER_UDS="$cache_dir/sccache.sock"
if ! mkdir -p -- "$SCCACHE_DIR" || ! sccache --start-server; then
    echo 'sccache server unavailable, building without compiler cache'
    exec {cache_lock}>&-
    exec "$@"
fi
# Stop the server before the shell releases the disk cache lock.
trap 'sccache --stop-server || true' EXIT
export RUSTC_WRAPPER=sccache CARGO_INCREMENTAL=0 SCCACHE_IGNORE_SERVER_IO_ERROR=1
"$@"
