# Rust Arch release package

The release workflow builds one `deskwaifu-bin-<tag>-1-x86_64.pkg.tar.zst` from the
exact selected Git tag. The workspace and `Cargo.lock` must live at repository
root. `cargo build --release --frozen` builds without updating the lockfile or
accessing the network; the current workspace has no external dependencies.
The package installs only `/usr/bin/deskwaifu`, with `glibc` and `gcc-libs` as
runtime dependencies. C++ plugins, private SDKs, OCR models and installed-test
packages are no longer packaged.

The fixed `deskwaifu-rust` runner provides Cargo, the system toolchain and
makepkg. A root orchestrator archives the selected source, then invokes makepkg
as a temporary unprivileged account with a clean environment. The account cannot
write to the checkout or trusted release-tool snapshot. Upload credentials exist
only in the final workflow step. Package identity, payload and checksums are
verified before upload; different existing remote assets are never overwritten.
Tag names are passed unchanged to makepkg (Arch-incompatible versions fail rather
than silently being renamed). Historical C++ tags are not supported by this Rust
packaging recipe. Notes-only updates still support existing releases.

The Rust workspace currently declares no license. The package therefore records
`LicenseRef-Unknown`, not the retired SDK's license; establish a project license
before public redistribution.

## AUR

The existing AUR submodule is deliberately unchanged until a new Rust release
has been published. Run `python3 scripts/update-aur.py <exact-tag>` after release:
it verifies public assets and their commit/checksums, migrates a legacy split
recipe using `packaging/aur-template/PKGBUILD`, and generates `.SRCINFO`. Review
and test that diff before committing the submodule and its parent pointer. It
refuses to replace the old layout under the same release tag. No workflow commits
to or automatically publishes the AUR repository.

## Verification

Run `python3 -B tests/test_release_packaging.py`. On Arch with makepkg, bsdtar,
fakeroot and Cargo, the nonroot integration test builds an offline Rust fixture
using the real PKGBUILD and verifies the package, manifest and upload checksums.
It is skipped where those prerequisites are unavailable.
