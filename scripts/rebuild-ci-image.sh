#!/usr/bin/env bash
# Build and publish the private Arch Linux CI image used by the Gitea runner.
set -euo pipefail

REGISTRY=git.kals.top
IMAGE_REPO=kals/deskwaifu-ci
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(dirname "$SCRIPT_DIR")"
CONTAINERFILE="$SOURCE_DIR/Containerfile"
PACMAN_MIRROR=${WAIFULAND_PACMAN_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/archlinux}
BOX_HOST=${WAIFULAND_BOX_HOST:-box}

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
IMAGE="$REGISTRY/$IMAGE_REPO:$TAG"

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
    pkgconf --modversion Qt6Core Qt6Widgets Qt6WaylandClient
    test -f /opt/waifuland-deps/CubismSdkForNative-5-r.5/Core/include/Live2DCubismCore.h
    test -f /opt/waifuland-deps/glew/include/GL/glew.h
    test ! -S /var/run/docker.sock
'

if ! podman login --get-login "$REGISTRY" >/dev/null 2>&1; then
    echo "Error: log in first with: podman login $REGISTRY --username kals" >&2
    exit 1
fi

echo "Pushing $IMAGE"
podman push "$IMAGE"

if DOCKER_DIGEST=$(ssh "$BOX_HOST" \
        "docker pull '$IMAGE' >/dev/null && docker image inspect '$IMAGE' --format '{{index .RepoDigests 0}}'"); then
    echo "Docker digest: $DOCKER_DIGEST"
    echo "Configure the runner label as:"
    echo "  deskwaifu-arch:docker://$DOCKER_DIGEST"
else
    echo "Warning: could not obtain the Docker digest from $BOX_HOST" >&2
fi
