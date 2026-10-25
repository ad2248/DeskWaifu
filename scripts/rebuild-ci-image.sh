#!/usr/bin/env bash
# 构建 Gitea 运行器使用的 Arch Linux CI 镜像，并私下传输。
set -euo pipefail

IMAGE_REPO=localhost/deskwaifu-ci
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(dirname "$SCRIPT_DIR")"
CONTAINERFILE="$SOURCE_DIR/Containerfile"
PACMAN_MIRROR=${DESKWAIFU_PACMAN_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/archlinux}
RUNNER_HOST=${DESKWAIFU_RUNNER_HOST:-st}

if ! command -v podman >/dev/null 2>&1; then
    echo "Error: podman is required" >&2
    exit 1
fi
if [[ ! -f $SOURCE_DIR/CubismSdkForNative-5-r.5/Core/include/Live2DCubismCore.h ]]; then
    echo "Error: CubismSdkForNative-5-r.5 is required in the repository root" >&2
    exit 1
fi
if [[ ! -f $SOURCE_DIR/thirdParty/glew/include/GL/glew.h ]]; then
    echo "Error: thirdParty/glew is required; run thirdParty/scripts/setup_glew" >&2
    exit 1
fi

CONTAINER_HASH=$(git -C "$SOURCE_DIR" hash-object Containerfile)
TAG="arch-$(date -u +%Y%m%d%H%M)-${CONTAINER_HASH:0:12}"
IMAGE="$IMAGE_REPO:$TAG"

echo "Building $IMAGE"
podman build --target ci \
    --build-arg "PACMAN_MIRROR=$PACMAN_MIRROR" \
    -t "$IMAGE" "$SOURCE_DIR"

echo "Checking the CI image"
podman run --rm "$IMAGE" bash -ceu '
    cmake --version
    ninja --version
    wayland-scanner --version
    sway --version
    QT_QPA_PLATFORM=offscreen kwin_wayland --version
    QT_QPA_PLATFORM=offscreen kscreen-doctor --help >/dev/null
    dbus-run-session -- true
    locale_dir=$(mktemp -d)
    localedef --no-archive -i de_DE -f UTF-8 "$locale_dir/de_DE.UTF-8"
    rm -rf "$locale_dir"
    pkgconf --modversion Qt6Core Qt6Widgets Qt6OpenGL Qt6WaylandClient Qt6StateMachine Qt6WebSockets Qt6Multimedia
    test -f /usr/lib/cmake/LayerShellQt/LayerShellQtConfig.cmake
    test -f /opt/deskwaifu-deps/CubismSdkForNative-5-r.5/Core/include/Live2DCubismCore.h
    test -f /opt/deskwaifu-deps/glew/include/GL/glew.h
    test ! -S /var/run/docker.sock
'

# Gitea 包继承所属用户的可见性，而非关联仓库的可见性。
# 不能将包含 SDK 的镜像推送到可能公开的用户命名空间。
IMAGE_ID=$(podman image inspect "$IMAGE" --format '{{.Id}}')
IMAGE_ID="sha256:${IMAGE_ID#sha256:}"
echo "Transferring $IMAGE privately to $RUNNER_HOST"
podman save --format oci-archive "$IMAGE" | ssh "$RUNNER_HOST" podman load
ssh "$RUNNER_HOST" podman image exists "$IMAGE_ID"
echo "Configure the runner label as (force_pull must remain false):"
echo "  deskwaifu-arch:docker://$IMAGE_ID"
