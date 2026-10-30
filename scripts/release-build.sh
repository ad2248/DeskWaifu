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
test "$(git rev-parse "refs/tags/$RELEASE_TAG^{commit}")" = "$RELEASE_COMMIT"
test -z "$(git status --porcelain --untracked-files=no)"
# /tmp is traversable by the temporary nonroot user; RUNNER_TEMP may be below
# a root-only directory. Do not relax permissions on that directory/workspace.
work=$(mktemp -d /tmp/deskwaifu-release.XXXXXXXX)
build_user=
cleanup() {
    if [[ -n "$build_user" ]]; then
        userdel "$build_user" || echo "Unable to remove temporary build user $build_user" >&2
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT
mkdir -p "$work/home" "$work/packages" "$work/private-deps"

# Acquisition stage: the only network download in this script.
# The private SDK/GLEW are already in the runner image, not fetched during build.
deps_dir=$(bash "$tools_root/scripts/find-deps-dir.sh")
cp -aL "$deps_dir/CubismSdkForNative-5-r.5" "$deps_dir/glew" "$work/private-deps/"
git archive --format=tar.gz --prefix=deskwaifu/ "$RELEASE_COMMIT" > "$work/deskwaifu.tar.gz"
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git show -s --format=%ct "$RELEASE_COMMIT")
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
    -czf "$work/private-deps.tar.gz" -C "$work" private-deps
ort=onnxruntime-linux-x64-1.31.0.tgz
curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
    "https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/$ort" -o "$work/$ort"
echo "cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57  $work/$ort" | sha256sum -c -
python3 "$tools_root/scripts/release-packaging.py" prepare "$work" "$RELEASE_TAG" "$RELEASE_COMMIT"

# Never grant the build user write access to the checkout or sudo. Only this new
# scratch directory is chowned; no broad chmod/chown of the runner workspace.
candidate="dwrel$$"
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
runuser -u "$build_user" -- env -i PATH=/usr/bin:/bin HOME="$work/home" SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    PKGDEST="$work/packages" CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
    bash -c 'cd "$1"; makepkg --cleanbuild --noconfirm' _ "$work"
output="$root/release-assets"
mkdir "$output" # Refuse stale artifacts from another tag/run.
cp "$work/packages/"*.pkg.tar.zst "$output/"
python3 "$tools_root/scripts/release-packaging.py" verify "$output" "$RELEASE_TAG" "$RELEASE_COMMIT"
