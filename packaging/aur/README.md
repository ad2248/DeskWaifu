# Arch Linux / AUR packaging

This directory contains the `deskwaifu-git` recipe for x86_64. It builds the
latest `main` commit, including the Qt control panel and the IPC CLI. It can be
used directly from this repository; publication on aur.archlinux.org is a
separate step.

The recipe fetches the application source from the public GitHub repository
at https://github.com/ad2248/DeskWaifu.git, tracking its `main` branch.
No Git credentials are required.

```bash
sudo pacman -S --needed base-devel git
git clone https://github.com/ad2248/DeskWaifu.git
cd DeskWaifu/packaging/aur
makepkg -si
```

Run `makepkg` as your regular user. It downloads the sources and uses pacman to
install missing dependencies and the resulting package. Cubism SDK 5-r.5 and
GLEW 2.3.1 archives have SHA-256 checksums; only the moving Git source uses
`SKIP`. Compilation and packaging do not perform additional downloads.

The SDK is downloaded directly from:
https://cubism.live2d.com/sdk-native/bin/CubismSdkForNative-5-r.5.zip

Review the [Live2D Proprietary Software License](https://www.live2d.com/eula/live2d-proprietary-software-license-agreement_en.html)
and [Live2D Open Software License](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html)
before building. These terms apply to the SDK/Core and SDK-derived code;
the SDK is not an open-source dependency. License notices are installed under
`/usr/share/licenses/deskwaifu-git/`.

## Running and updating

Use a Wayland compositor supporting `wlr-layer-shell`, such as Hyprland or Sway.
Place your models in `~/.config/deskwaifu/models/` (or
`$XDG_CONFIG_HOME/deskwaifu/models/`), then run:

```bash
deskwaifu &
deskwaifu-control
```

The **DeskWaifu Control** application-menu entry opens the control panel. Start
`deskwaifu` first; the panel does not launch the backend. The package does not
include models or configure autostart. User configuration remains in
`~/.config/deskwaifu/` (or `$XDG_CONFIG_HOME/deskwaifu/`).

To update, run `git pull` in this checkout and `makepkg -si` again in this
directory. To uninstall: `sudo pacman -Rns deskwaifu-git`.

## Maintaining / publishing the AUR recipe

Copy `PKGBUILD` and `.SRCINFO` to the root of the AUR package repository.
Before publishing, the application installation rules must have reached `main`.
Refresh metadata after changing the recipe:

```bash
makepkg --printsrcinfo > .SRCINFO
```

Validate with `makepkg`, `namcap PKGBUILD`, and `namcap` on the resulting
package. `check()` runs CTest with Qt's offscreen backend and validates the
desktop entry. `package()` uses the `DeskWaifu` CMake install component to avoid
shipping bundled GLEW development files. The SDK and sample models are build
inputs only; they are not installed as SDK files.
