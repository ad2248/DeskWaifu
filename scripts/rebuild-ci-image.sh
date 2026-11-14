#!/usr/bin/env bash
# Build, validate and privately transfer the Rust CI image.
set -euo pipefail
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNNER_HOST=${DESKWAIFU_RUNNER_HOST:-st}
command -v podman >/dev/null
hash=$(git -C "$SOURCE_DIR" hash-object Containerfile)
image="localhost/deskwaifu-rust-ci:arch-$(date -u +%Y%m%d%H%M)-${hash:0:12}"
podman build --target ci --build-arg "PACMAN_MIRROR=${DESKWAIFU_PACMAN_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/archlinux}" -t "$image" "$SOURCE_DIR"
podman run --rm "$image" bash -ceu '
    rustc --version
    cargo --version
    cargo fmt --version
    cargo clippy --version
    makepkg --version
    python3 --version
    node --version
    git --version
    bsdtar --version
    command -v fakeroot runuser useradd userdel
    test ! -S /var/run/docker.sock
    scratch=$(mktemp -d)
    cd "$scratch"
    cargo new --bin --vcs none check-image
    cd check-image
    cargo generate-lockfile --offline
    cargo fmt --all --check
    cargo clippy --frozen --all-targets -- -D warnings
    cargo test --frozen
    cargo build --frozen --release
'
image_id=$(podman image inspect "$image" --format '{{.Id}}')
image_id="sha256:${image_id#sha256:}"
podman save --format oci-archive "$image" | gzip -1 | ssh "$RUNNER_HOST" 'gzip -d | podman load'
ssh "$RUNNER_HOST" podman image exists "$image_id"
echo "Rust runner label (keep force_pull: false): deskwaifu-rust:docker://$image_id"
