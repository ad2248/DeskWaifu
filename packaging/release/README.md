# Rust Arch release package

The release workflow builds one `deskwaifu-bin-<tag>-1-x86_64.pkg.tar.zst` from the
exact selected Git tag. The workspace and `Cargo.lock` must live at repository
root. The recipe calls `CARGO_NET_OFFLINE=true cargo build --workspace --locked
--release` with an explicit target directory and installs directly from Cargo's
`target/release/` outputs. The ordinary release build first runs `cargo fetch
--locked` as the temporary build user against a separate extraction of the same
source archive. Fetch and makepkg share a fresh `CARGO_HOME`; only fetch receives
allowlisted proxy settings. CI instead uses the locked Cargo sources provisioned
in the runner image. Compilation never runs as root and no upload credentials
reach either phase.

The trusted release tools select the payload using the selected source archive's
root workspace declarations:

| Payload | Workspace marker | Installed components beyond the host and CLI |
| --- | --- | --- |
| `rust-v1` | `plugins/test-plugin` | `libdeskwaifu_test_plugin.so` |
| `rust-v2` | `plugins/voice-input` | v1 plus `libdeskwaifu_voice_input.so` |
| `rust-v3` | `apps/terminal-panel` | v2 plus native `libdeskwaifu_terminal.so` and `deskwaifu-terminal-panel` |

The host is installed at `/usr/lib/deskwaifu/deskwaifu`, with a symlink at
`/usr/bin/deskwaifu`. The independent CLI and, for v3, the panel are installed
in `/usr/bin/`. The plugin loader searches `plugins/` beside the actual host
executable, not beside the symlink or in the working directory. Installation and
verification use the same artifact list; missing outputs and unexpected files
are rejected. The package does not install model assets or separate SDK files.

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
than silently being renamed). Builds require a supported Rust workspace. Notes-only updates do not rebuild assets.

The package records `LicenseRef-Unknown` because the workspace does not declare a license.

## Native build inputs for `rust-v3`

Build v3 with a recent Rust toolchain and the Arch base-devel toolchain, CMake,
and the declared runtime libraries. V3 depends on `libglvnd` and
`systemd-libs` for the native terminal and its built-in KWin cursor support,
plus `wayland`, `fontconfig`, `freetype2`, `libxkbcommon`, `libxkbcommon-x11`,
`libxcb`, `libx11`, and `vulkan-icd-loader` for the panel. A suitable Vulkan
driver is optional. See [terminal-panel](<../../apps/terminal-panel/README.md>)
for panel usage.

Provision Cubism Native SDK 5-r.5 and GLEW sources outside the source archive.
Set `CUBISM_SDK_DIR` and `CUBISM_GLEW_DIR` to their absolute directories in the
environment of the [release build step](<../../scripts/release-build.sh>).
The temporary makepkg user must be able to read the inputs and traverse their
paths. Only those two SDK variables are added to makepkg's clean environment,
and only for v3; neither is passed to Cargo fetch. V1 and v2 do not require
these inputs. The SDK is used at build time and Cubism code is linked into the
native plugin, but the package ships no separate SDK files or sample models.
Supply licensed models on the target machine. Before enabling public v3
releases, confirm that the project and applicable Live2D terms permit
redistributing the compiled plugin, then set the Gitea repository variable
`DESKWAIFU_CUBISM_REDISTRIBUTION_APPROVED` to `1`. Without that explicit opt-in,
the public release workflow stops before building or uploading v3. Private
builds, pre-v3 releases and notes-only updates are unaffected.

## AUR

Run `python3 scripts/update-aur.py <exact-tag>` after release:
it verifies public assets and their commit/checksums, upgrades split-package input
with [the AUR template](<../aur-template/PKGBUILD>), and generates `.SRCINFO`.
For v3, declare the full runtime dependencies listed above in the consumer recipe
or the template used for split-package input; the updater rejects missing dependencies.
Review and test the generated diff before committing the submodule and its parent
pointer. A package layout change requires a different release tag. No
workflow commits to or automatically publishes the AUR repository.

## Verification

Run `python3 -B tests/test_release_packaging.py`. The tests exercise fetch/build
phase isolation, the public v3 opt-in, and real offline Cargo builds of three
workspace fixtures using the recipe's build/package functions. They verify the
installed host symlink, ELF plugins, CLI `--help`, and v3 panel binary, and reject
missing outputs or unexpected SDK and model files.
On Arch with makepkg, bsdtar, fakeroot and Cargo, an additional nonroot integration
test creates the v3 fixture package and verifies its payload, manifest and upload
checksums. Only the Arch-specific tests are skipped when those tools are absent.
