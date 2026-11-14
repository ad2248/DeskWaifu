#!/usr/bin/env bash
set -euo pipefail

for deps_dir in /opt/*-deps; do
    if [[ -d "$deps_dir/CubismSdkForNative-5-r.5/Samples/Resources" &&
          -d "$deps_dir/glew" ]]; then
        printf '%s\n' "$deps_dir"
        exit 0
    fi
done

echo "No /opt/*-deps directory with CubismSdkForNative-5-r.5 and glew found" >&2
exit 1
