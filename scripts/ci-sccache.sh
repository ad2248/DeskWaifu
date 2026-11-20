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

export SCCACHE_DIR="$cache_dir" SCCACHE_CACHE_SIZE=4G
if ! sccache --start-server; then
    echo 'sccache server unavailable, building without compiler cache'
    exec {cache_lock}>&-
    exec "$@"
fi
# Stop the server before the shell releases the disk cache lock.
trap 'sccache --stop-server || true' EXIT
export RUSTC_WRAPPER=sccache CARGO_INCREMENTAL=0 SCCACHE_IGNORE_SERVER_IO_ERROR=1
"$@"
