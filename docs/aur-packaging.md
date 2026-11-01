# AUR packaging and maintenance

## Ownership

- `packaging/aur/`: Git submodule for
  `ssh://aur@aur.archlinux.org/deskwaifu-bin.git`. Its split `PKGBUILD` consumes
  GitHub binary assets and outputs `deskwaifu-bin` and `deskwaifu-ocr-bin`.
- `packaging/arch/deskwaifu-git/`: local source recipe, tracking GitHub `main`.
  This is not a second published AUR repository.
- `packaging/release/PKGBUILD.in`, `scripts/release-*`, and
  `.gitea/workflows/release.yml`: binary production and GitHub upload. These
  remain in the main repository and do not require the AUR submodule.
- `scripts/update-aur.py`: verifies a published release and edits the AUR
  recipe. It does not build binaries, commit, or push.

Ordinary clones, source builds and CI do not need recursive submodule checkout.
Maintainers initialize the submodule using their AUR SSH key:

```bash
git submodule update --init packaging/aur
```

For read-only initialization without AUR SSH credentials:

```bash
git -c submodule.packaging/aur.url=https://aur.archlinux.org/deskwaifu-bin.git \
  submodule update --init packaging/aur
```

Do not expose the AUR private key to PR jobs, builds or dependency acquisition.

## Updating a release

1. Finish the existing Gitea release workflow first. Both binary packages,
   `release-metadata.json`, and `SHA256SUMS` must be publicly downloadable.
2. Check out the latest AUR branch and ensure the submodule is clean. Run as
   an ordinary user with Python, makepkg and bsdtar installed:
   `python3 scripts/update-aur.py 0.1.0` (replace with the exact tag).
3. Review the diff. The updater checks the GitHub tag's commit against the
   manifest, SHA-256, binary package identities, dependencies and payload contract.
   An existing release with different checksums is rejected.
   If the release requires dependencies missing from the generated AUR metadata,
   the update is rejected before modifying either maintained file. Adjust the
   recipe manually, regenerate `.SRCINFO`, and commit those changes before retrying.
4. In a clean Arch environment, run `makepkg -s`, `namcap PKGBUILD`, and
   `namcap` on each generated package. Test the main package alone and then OCR.
   Do not use `makepkg -i` when only the main split package is wanted.
5. Commit and push the AUR repository first, then commit its new pointer in the
   main repository. Do not publish a parent pointer that AUR cannot serve.

```bash
git -C packaging/aur add PKGBUILD .SRCINFO
git -C packaging/aur commit -m 'feat(aur): 更新二进制发行版本'
git -C packaging/aur push origin master
git add packaging/aur
git commit -m 'chore(aur): 更新子模块版本'
```

GitHub tags currently map unchanged to `pkgver`, and upstream artifacts use
`pkgrel=1`. Unsupported tag syntax is rejected rather than silently normalized.
The AUR `pkgrel` is independent: bump it for recipe-only fixes and regenerate
`.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`. OCR must depend on the AUR
main package's actual `pkgver-pkgrel`, not the upstream artifact's release number.
A new upstream version resets AUR `pkgrel` to 1; updating the same release keeps it.

Downloads use an `upstream-` filename prefix to avoid colliding with makepkg's
output packages. `noextract` prevents the two upstream archives overwriting each
other; each package extracts only its own `usr/` payload. Upstream package
metadata is not installed, and stripping/debug splitting is disabled.
The OCR license directory is renamed from the upstream component name
`deskwaifu-ocr` to the installed package name `deskwaifu-ocr-bin`.

Known namcap findings inherited from the published payload: `/usr/libexec/`
is intentional because the application invokes OCR there; relocating it in the
AUR recipe would break that contract. Runtime-loaded Qt Wayland components,
external helper dependencies and gcc-libs may be reported as unnecessary by
static inspection and are deliberately retained to match the release contract.

## Failure and migration safety

- Incomplete GitHub releases or failed checks must not change the AUR recipe.
- Failed AUR pushes can be retried without rebuilding or replacing GitHub assets.
- Keep GitHub releases immutable; issue a new release rather than replacing files.
- Never force-push existing AUR history.
- Before replacing an existing `packaging/aur/` checkout, inspect tracked changes
  and ignored build artifacts. Move or remove caches only with the owner's consent.
- Verify redistribution rights for Live2D Core/SDK-derived binaries before public
  publication. Existing release assets are not proof of licensing permission.

Installing or switching packages must not remove models or user configuration,
and must not enable autostart. The source and binary variants conflict rather
than being installed over each other. OCR models/dictionaries are provisioned
separately and never included in these packages.
