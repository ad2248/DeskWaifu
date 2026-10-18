#!/usr/bin/env bash
set -euo pipefail
SOURCE_DIR=$1
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/deskwaifu-ci-transfer.XXXXXX")
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$TEST_DIR/scripts" "$TEST_DIR/bin" \
    "$TEST_DIR/CubismSdkForNative-5-r.5/Core/include" "$TEST_DIR/thirdParty/glew/include/GL"
cp "$SOURCE_DIR/scripts/rebuild-ci-image.sh" "$TEST_DIR/scripts/"
touch "$TEST_DIR/Containerfile" \
    "$TEST_DIR/CubismSdkForNative-5-r.5/Core/include/Live2DCubismCore.h" \
    "$TEST_DIR/thirdParty/glew/include/GL/glew.h"
export CI_TRANSFER_LOG="$TEST_DIR/commands.log" CI_TRANSFER_DATA="$TEST_DIR/transferred"
cat >"$TEST_DIR/bin/podman" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf 'podman %s\n' "$*" >>"$CI_TRANSFER_LOG"
case "$1" in
    build|run) ;;
    image) printf '%064d\n' 1 ;;
    save) printf 'private-sdk-image' ;;
    *) exit 1 ;; # In particular, do not log in, push or publish a package.
esac
SH
cat >"$TEST_DIR/bin/ssh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf 'ssh %s\n' "$*" >>"$CI_TRANSFER_LOG"
[[ $1 == ci-host ]]
shift
case "$*" in
    'podman load') cat >"$CI_TRANSFER_DATA" ;;
    "podman image exists sha256:$(printf '%064d' 1)") ;;
    *) exit 1 ;;
esac
SH
chmod +x "$TEST_DIR/bin/podman" "$TEST_DIR/bin/ssh"
PATH="$TEST_DIR/bin:$PATH" DESKWAIFU_RUNNER_HOST=ci-host \
    bash "$TEST_DIR/scripts/rebuild-ci-image.sh" >"$TEST_DIR/result"
[[ $(cat "$CI_TRANSFER_DATA") == private-sdk-image ]]
grep -q 'podman build .*localhost/deskwaifu-ci:arch-' "$CI_TRANSFER_LOG"
grep -q 'podman save --format oci-archive localhost/deskwaifu-ci:arch-' "$CI_TRANSFER_LOG"
grep -q "deskwaifu-arch:docker://sha256:$(printf '%064d' 1)" "$TEST_DIR/result"
if grep -Eq 'podman (push|login)|docker pull|git.kals.top/kals/deskwaifu-ci' "$CI_TRANSFER_LOG"; then
    echo 'CI image must not be published to a public registry namespace' >&2
    exit 1
fi
echo 'Private CI image transfer and immutable image-ID label passed'
