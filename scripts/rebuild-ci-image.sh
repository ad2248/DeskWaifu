#!/usr/bin/env bash
# Build, validate and privately transfer the Rust CI image.
set -euo pipefail
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNNER_HOST=${DESKWAIFU_RUNNER_HOST:-st}
command -v podman >/dev/null
hash=$(git -C "$SOURCE_DIR" hash-object Containerfile)
image="localhost/deskwaifu-rust-ci:arch-$(date -u +%Y%m%d%H%M)-${hash:0:12}"
podman build --target ci \
    --build-arg "PACMAN_MIRROR=${DESKWAIFU_PACMAN_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/archlinux}" \
    --build-arg "CARGO_HTTPS_PROXY=${DESKWAIFU_CARGO_HTTPS_PROXY:-http://100.64.0.11:7912}" \
    -t "$image" "$SOURCE_DIR"
podman run --rm --network none -v "$SOURCE_DIR:/source:ro" "$image" bash -ceu '
    export CARGO_HOME=$(mktemp -d)
    install -m 0644 /opt/deskwaifu-cargo/config.toml "$CARGO_HOME/config.toml"
    cargo metadata --frozen --manifest-path /source/Cargo.toml --format-version 1 >/dev/null
    rustc --version
    cargo --version
    cargo fmt --version
    cargo clippy --version
    makepkg --version
    python3 --version
    node --version
    git --version
    bsdtar --version
    clang --version
    sccache --version
    command -v flock mountpoint
    pkg-config --atleast-version=0.3.44 libpipewire-0.3
    pkg-config --exists libsystemd wayland-client freetype2 fontconfig xkbcommon xkbcommon-x11 xcb x11 vulkan
    wl-copy --version
    wl-paste --version
    command -v fakeroot runuser useradd
    cmake --version
    export CARGO_TARGET_DIR=$(mktemp -d)
    cd /source
    cargo build --workspace --frozen
    test -x "$CARGO_TARGET_DIR/debug/deskwaifu-terminal-panel"
    terminal_plugin="$CARGO_TARGET_DIR/debug/libdeskwaifu_terminal.so"
    test -f "$terminal_plugin"
    # Verify the default plugin links both the native renderer and KWin sd-bus bridge.
    terminal_libraries=$(LC_ALL=C readelf -d "$terminal_plugin")
    grep -E "\(NEEDED\).*\[libEGL\.so\.1\]" <<< "$terminal_libraries"
    grep -E "\(NEEDED\).*\[libsystemd\.so\.0\]" <<< "$terminal_libraries"
    env -u DISPLAY -u WAYLAND_DISPLAY -u CUBISM_TEST_PCI -u CUBISM_TEST_SECOND_PCI \
        LIBGL_ALWAYS_SOFTWARE=1 cargo test --frozen -p cubism-bridge --features native --test offscreen
    for model in Haru Hiyori Mao; do
        env -u DISPLAY -u WAYLAND_DISPLAY LIBGL_ALWAYS_SOFTWARE=1 \
            cargo run --frozen -p cubism-bridge --features native --example offscreen -- \
            "$CUBISM_SDK_DIR/Samples/Resources/$model/$model.model3.json" "$CARGO_TARGET_DIR/$model.png"
        test -s "$CARGO_TARGET_DIR/$model.png"
    done
    bash scripts/with-headless-plasma.sh true
    unset CARGO_TARGET_DIR
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
