# Rust Arch release package

The release workflow builds one `deskwaifu-bin-<tag>-1-x86_64.pkg.tar.zst` from the
exact selected Git tag. The workspace and `Cargo.lock` must live at repository
root. `CARGO_NET_OFFLINE=true bash scripts/build.sh --release` builds the whole
workspace with `--locked` without updating the lockfile or accessing the network;
beforehand, `release-build.sh` runs `cargo fetch --locked` as the temporary build
user against a separate extraction of the selected source archive. Fetch and
makepkg share the same fresh `CARGO_HOME`; no dependency cache is required in the
runner image. Only the fetch phase receives allowlisted proxy settings. The
subsequent makepkg invocation has `CARGO_NET_OFFLINE=true` and no proxy settings;
compilation never runs as root and no upload credentials reach either phase.
The package installs `/usr/lib/deskwaifu/deskwaifu` and its jointly built Rust
plugins under `/usr/lib/deskwaifu/plugins/`. The trusted release tools select a
payload from the selected source archive's Cargo workspace: `rust-v1` includes
`libdeskwaifu_test_plugin.so`; `rust-v2` requires that plugin and
`libdeskwaifu_voice_input.so` when the workspace declares `plugins/voice-input`.
Installation and verification use the same plugin list and fail if an expected
artifact is missing. `/usr/bin/deskwaifu` is symlinked to
`../lib/deskwaifu/deskwaifu`, and the independent CLI is installed at
`/usr/bin/deskwaifucli`. The loader searches `plugins/` beside the actual
executable, not beside the symlink or in the working directory. Runtime
dependencies are `glibc`, `gcc-libs`, `libpipewire` and `wl-clipboard`. C++ plugins,
private SDKs, OCR models and installed-test packages are no longer packaged.

Host and plugins must be built together from the same code with the same Rust
toolchain. The plugin entry point uses Rust ABI, not a stable C ABI; there is no
version compatibility check or sandbox. Load only trusted plugins. Plugins load
at startup and unload on shutdown; hot reload is not supported.

Use `/usr/bin/deskwaifucli --help` for a service-independent installation check.
The host `deskwaifu` accepts no arguments (not even `--help`); invocation without
arguments starts the persistent host and will not exit. To exercise a plugin,
start that host and use `deskwaifucli test` in another terminal (the IPC socket is
fixed at `/tmp/deskwaifu.socket`).

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

Run `python3 -B tests/test_release_packaging.py`. The tests exercise fetch/build
phase isolation and a real offline Rust host + CLI + cdylib fixture using the
PKGBUILD's build/package functions, including the installed host symlink, ELF
plugin, independent CLI and its `--help`.
On Arch with makepkg, bsdtar, fakeroot and Cargo, an additional nonroot integration
test creates the actual package and verifies its payload, manifest and upload
checksums. Only the Arch-specific tests are skipped when those tools are absent.
