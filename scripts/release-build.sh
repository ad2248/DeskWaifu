#!/usr/bin/env bash
# 打包入口：root 编排，用固定的非 root 账户运行 makepkg。
# CI 的 build 作业和发版工作流共用；调用前需设置 RELEASE_TAG 与 RELEASE_COMMIT。
set -euo pipefail

if [[ $(id -u) != 0 ]]; then
    echo '打包需要 root 编排，makepkg 由非 root 账户执行' >&2
    exit 1
fi

# Run only after release-source.py. The checkout is read-only to the build user.
: "${RELEASE_TAG:?}" "${RELEASE_COMMIT:?}"
root=$(pwd -P)
tools_root=${RELEASE_TOOLS_DIR:-$root}
test "$(git rev-parse HEAD)" = "$RELEASE_COMMIT"
# Release tag selection is enforced by release-source.py. CI may package an
# untagged commit with an explicit package version using this same entry point.
if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
    echo '工作区有未提交的改动，打包只接受干净检出' >&2
    exit 1
fi

# Fixed work path so sccache hits across runs; the lock serialises concurrent builds.
work=/tmp/deskwaifu-package
exec 9>/tmp/deskwaifu-package.lock
flock 9
rm -rf -- "$work"
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/home" "$work/packages"

# The fixed runner image supplies Cargo and the system toolchain. Fetch locked
# dependencies as the build user before the offline build of this exact archive.
git archive --format=tar.gz --prefix=deskwaifu/ "$RELEASE_COMMIT" > "$work/deskwaifu.tar.gz"
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git show -s --format=%ct "$RELEASE_COMMIT")
# Retain the source-selected payload in this shell, not in the build-user directory.
payload=$(python3 "$tools_root/scripts/release-packaging.py" prepare "$work" "$RELEASE_TAG" "$RELEASE_COMMIT")

build_user=${DESKWAIFU_BUILD_USER:-builder}
id -u "$build_user" >/dev/null 2>&1 ||
    useradd --uid 2000 --user-group --no-create-home --home-dir "$work/home" --shell /bin/bash "$build_user"
chown -R "$build_user:$build_user" "$work"

# Public release builds must be authorized before fetching or compiling v3.
if [[ $payload == rust-v3 && ${DESKWAIFU_PUBLIC_RELEASE:-0} == 1 &&
      ${DESKWAIFU_CUBISM_REDISTRIBUTION_APPROVED:-0} != 1 ]]; then
    echo 'Publishing rust-v3 requires confirmed Cubism redistribution authorization' >&2
    exit 1
fi
# Only the rust-v3 payload requires local Cubism and GLEW inputs.
sdk_env=()
if [[ $payload == rust-v3 ]]; then
    for key in CUBISM_SDK_DIR CUBISM_GLEW_DIR; do
        input=${!key:-}
        if [[ $input != /* || ! -d $input ]]; then
            echo "Set $key to an absolute directory" >&2
            exit 1
        fi
        sdk_env+=("$key=$input")
    done
fi
# Offline CI uses the runner image's Cargo sources. Otherwise fetch the selected
# archive's locked dependencies before entering the offline package build.
if [[ ${DESKWAIFU_CI_OFFLINE:-0} == 1 ]]; then
    test -r /opt/deskwaifu-cargo/config.toml || {
        echo 'CI image is missing Cargo dependencies. Rebuild the runner image for this Cargo.lock.' >&2
        exit 1
    }
    install -d -o "$build_user" "$work/home/.cargo"
    install -m 0644 /opt/deskwaifu-cargo/config.toml "$work/home/.cargo/config.toml"
else
    # Only this dependency-acquisition phase may use the network. Fetch executes
    # no build scripts and uses the same CARGO_HOME as the offline build.
    runuser -u "$build_user" -- env -i PATH=/usr/bin:/bin HOME="$work/home" \
        CARGO_HOME="$work/home/.cargo" \
        http_proxy="${http_proxy:-${HTTP_PROXY:-}}" https_proxy="${https_proxy:-${HTTPS_PROXY:-}}" \
        ALL_PROXY="${ALL_PROXY:-${all_proxy:-}}" no_proxy="${no_proxy:-${NO_PROXY:-}}" \
        bash -euc '
            cd "$1"
            mkdir fetch-source
            tar -xzf deskwaifu.tar.gz -C fetch-source
            cargo fetch --locked --manifest-path fetch-source/deskwaifu/Cargo.toml
            rm -rf fetch-source
        ' _ "$work"
fi
build_command=(makepkg --cleanbuild --noconfirm)
if [[ ${DESKWAIFU_CI_SCCACHE:-0} == 1 && -z ${RELEASE_TOOLS_DIR:-} ]] && mountpoint -q /cache; then
    cache_dir=/cache/sccache-rust-package
    if install -d -m 0775 -o "$build_user" -g "$build_user" -- "$cache_dir"; then
        install -m 0755 "$tools_root/scripts/ci-sccache.sh" "$work/ci-sccache.sh"
        build_command=(bash "$work/ci-sccache.sh" "$cache_dir" "${build_command[@]}")
    else
        echo 'sccache cache directory unavailable, building without compiler cache'
    fi
fi
runuser -u "$build_user" -- env -i PATH=/usr/bin:/bin HOME="$work/home" \
    SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" TZ=UTC LANG=C.UTF-8 \
    PKGDEST="$work/packages" CARGO_HOME="$work/home/.cargo" CARGO_NET_OFFLINE=true "${sdk_env[@]}" \
    bash -euc 'cd "$1"; shift; exec "$@"' _ "$work" "${build_command[@]}"
output="$root/release-assets"
mkdir "$output" # Refuse stale artifacts from another tag/run.
cp "$work/packages/"*.pkg.tar.zst "$output/"
python3 "$tools_root/scripts/release-packaging.py" verify "$output" "$RELEASE_TAG" "$RELEASE_COMMIT" --payload "$payload"
