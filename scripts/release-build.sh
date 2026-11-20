#!/usr/bin/env bash
set -euo pipefail

if [[ $(id -u) != 0 ]]; then
    echo 'Release builds require root orchestration and a separate unprivileged build user (no sudo fallback)' >&2
    exit 1
fi

# Run only after release-source.py. The checkout is read-only to the build user.
: "${RELEASE_TAG:?}" "${RELEASE_COMMIT:?}"
root=$(pwd -P)
tools_root=${RELEASE_TOOLS_DIR:-$root}
test "$(git rev-parse HEAD)" = "$RELEASE_COMMIT"
# Release tag selection is enforced by release-source.py. CI may package an
# untagged commit with an explicit package version using this same entry point.
test -z "$(git status --porcelain --untracked-files=no)"
# /tmp is traversable by the temporary nonroot user; RUNNER_TEMP may be below
# a root-only directory. Do not relax permissions on that directory/workspace.
# Each job has its own container. A stable path/user also stabilizes ELF paths
# and makepkg's .BUILDINFO; never reuse a directory left by another invocation.
work=/tmp/deskwaifu-package
mkdir "$work"
build_user=
cleanup() {
    if [[ -n "$build_user" ]]; then
        userdel "$build_user" || echo "Unable to remove temporary build user $build_user" >&2
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT
mkdir -p "$work/home" "$work/packages"

# The fixed runner image supplies Cargo and the system toolchain. Fetch locked
# dependencies as the build user before the offline build of this exact archive.
git archive --format=tar.gz --prefix=deskwaifu/ "$RELEASE_COMMIT" > "$work/deskwaifu.tar.gz"
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git show -s --format=%ct "$RELEASE_COMMIT")
# Retain the source-selected payload in this shell, not in the build-user directory.
payload=$(python3 "$tools_root/scripts/release-packaging.py" prepare "$work" "$RELEASE_TAG" "$RELEASE_COMMIT")

# Never grant the build user write access to the checkout or sudo. Only this new
# scratch directory is chowned; no broad chmod/chown of the runner workspace.
candidate=dwrel
useradd --system --user-group --no-create-home --home-dir "$work/home" "$candidate"
# Record ownership only AFTER successful creation. A collision must never
# cause cleanup to delete an account that existed before this invocation.
build_user=$candidate
chown -R "$build_user:$build_user" "$work"
# Fail rather than changing checkout permissions if this runner exposes trusted
# scripts as writable to the build account. They execute later with upload access.
for trusted in "$root" "$tools_root" "$tools_root/scripts" "$tools_root/scripts/release-"*.py "$tools_root/scripts/release-build.sh"; do
    runuser -u "$build_user" -- test ! -w "$trusted" || {
        echo "Build user can modify trusted release tooling: $trusted" >&2
        exit 1
    }
done
# A writable non-sticky parent would allow replacement of a read-only tree.
for trusted in "$root" "$tools_root"; do
    while [[ $trusted != / ]]; do
        parent=$(dirname "$trusted")
        if runuser -u "$build_user" -- test -w "$parent" && [[ ! -k $parent ]]; then
            echo "Build user can replace trusted release tooling through: $parent" >&2
            exit 1
        fi
        trusted=$parent
    done
done
# Only this dependency-acquisition phase may use the network. Fetch executes no
# compilation/build scripts, runs without root or upload credentials, and fills
# the same fresh CARGO_HOME that makepkg uses. Forward only proxy configuration.
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
build_command=(makepkg --cleanbuild --noconfirm)
if [[ ${DESKWAIFU_CI_SCCACHE:-0} == 1 && -z ${RELEASE_TOOLS_DIR:-} ]] && mountpoint -q /cache; then
    cache_dir=/cache/sccache-rust-package
    if [[ ! -L "$cache_dir" ]] && install -d -m 0755 -o "$build_user" -- "$cache_dir"; then
        install -m 0644 "$tools_root/scripts/ci-sccache.sh" "$work/ci-sccache.sh"
        build_command=(bash "$work/ci-sccache.sh" "$cache_dir" "${build_command[@]}")
    else
        echo 'sccache cache directory unavailable, building without compiler cache'
    fi
fi
runuser -u "$build_user" -- env -i PATH=/usr/bin:/bin HOME="$work/home" SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" TZ=UTC LANG=C.UTF-8 \
    PKGDEST="$work/packages" CARGO_HOME="$work/home/.cargo" CARGO_NET_OFFLINE=true \
    bash -euc 'cd "$1"; shift; exec "$@"' _ "$work" "${build_command[@]}"
output="$root/release-assets"
mkdir "$output" # Refuse stale artifacts from another tag/run.
cp "$work/packages/"*.pkg.tar.zst "$output/"
python3 "$tools_root/scripts/release-packaging.py" verify "$output" "$RELEASE_TAG" "$RELEASE_COMMIT" --payload "$payload"
