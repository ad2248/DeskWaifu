# DeskWaifu

> **Disclaimer:** This repository is "vibe coded". Please use with caution.

A Linux desktop Live2D model viewer that renders Live2D characters as transparent Wayland overlay windows using the `wlr-layer-shell` protocol.

Live2D models float on your desktop with click-through transparency — only the model itself receives input. Built with the Live2D Cubism SDK for Native, OpenGL (EGL), and native Wayland (no X11, no GLFW windowing).

<!-- Screenshot or GIF placeholder: place a demo image/gif here -->
<!-- ![DeskWaifu Demo](docs/demo.gif) -->


https://github.com/user-attachments/assets/95dfdedc-4957-4bb7-b79f-f3addd97a6db



## Table of Contents

- [Features](#features)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Upgrading from Waifuland](#upgrading-from-waifuland)
- [Usage](#usage)
- [IPC API](#ipc-api)
- [Model Setup](#model-setup)
- [Architecture](#architecture)
- [Built With](#built-with)
- [License](#license)

## Features

- **Wayland-native overlay** — renders as a layer-shell surface, always on top of your desktop
- **Click-through transparency** — only the Live2D model area receives pointer events; everything else passes through
- **Multi-compositor support** — works on any Wayland compositor supporting `wlr-layer-shell` (Hyprland, Sway, river, etc.)
- **Multi-output support** — switch between monitors (Hyprland)
- **Interactive** — responds to mouse drag, tap, and scroll input
- **Motion & expression** — supports idle animations, lip-sync, eye-blink, physics, and expressions
- **Configurable model directory** — load models from any path via CLI flag or XDG config
- **JSON configuration** — customize behavior via `config.json` (default model, emotion timeout, additional model dirs, scale/position, window size)
- **IPC control** — Unix domain socket API for external control (model switching, expressions, motions, lipsync, zoom, position, look-at direction)

## Prerequisites

| Dependency | Notes |
|---|---|
| **Linux with Wayland compositor** | Must support `wlr-layer-shell-v1` (Hyprland, Sway, river, etc.) |
| **Live2D Cubism SDK for Native** | Download from [live2d.com/sdk](https://www.live2d.com/en/sdk/about/) (proprietary, not bundled) |
| **C++ compiler** | GCC or Clang with C++14 support |
| **CMake** | >= 3.16 |
| **pkg-config** | For finding Wayland/EGL libraries |
| **Wayland development libraries** | `wayland-client`, `wayland-egl`, `wayland-cursor` |
| **EGL & OpenGL** | `libegl-dev`, `libgl-dev` or equivalent |
| **wayland-scanner** | Usually part of `wayland-protocols` or `wayland` dev packages |
| **Qt 6** | `Widgets`, `Network`, and the Qt Wayland platform plugin for the control window |
| **curl, unzip** | For downloading third-party dependencies (GLEW) |

### Installing system dependencies

**Arch Linux:**
```bash
sudo pacman -S --needed base-devel cmake pkgconf wayland wayland-protocols libglvnd egl-wayland qt6-base qt6-wayland curl unzip
```

**Ubuntu / Debian:**
```bash
sudo apt install build-essential cmake pkg-config libwayland-dev wayland-protocols libegl-dev libgl-dev libglu1-mesa-dev qt6-base-dev qt6-wayland curl unzip
```

**Fedora:**
```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config wayland-devel wayland-protocols-devel mesa-libEGL-devel mesa-libGL-devel qt6-qtbase-devel qt6-qtwayland curl unzip
```

## Installation

### Arch Linux package (makepkg)

An AUR-style `deskwaifu-git` recipe is available in `packaging/aur/`:

```bash
sudo pacman -S --needed base-devel git
git clone https://github.com/ad2248/DeskWaifu.git
cd DeskWaifu/packaging/aur
makepkg -si
```

This builds the latest `main` for x86_64 and installs `deskwaifu`,
`deskwaifu-control`, `deskwaifu-ctl`, and a control-panel application-menu entry.
The SDK and GLEW are downloaded automatically. See
[packaging instructions and license information](packaging/aur/README.md).
The recipe is usable locally; this does not imply it has been published to AUR.

### Building from source

#### 1. Clone the repository

```bash
git clone https://git.kals.top/kals/DeskWaifu.git
cd DeskWaifu
```

#### 2. Download the Live2D Cubism SDK

1. Go to [https://www.live2d.com/en/sdk/about/](https://www.live2d.com/en/sdk/about/)
2. Download **Cubism SDK for Native**
3. Extract it into the project root directory:
   ```
   DeskWaifu/
   ├── CubismSdkForNative-5-r.5/   <-- extracted SDK here
   ├── CMakeLists.txt
   ├── src/
   └── ...
   ```
4. If the folder name differs from `CubismSdkForNative-5-r.5`, update `SDK_ROOT_PATH` in `CMakeLists.txt`.

#### 3. Build

Use the install script (recommended):

```bash
chmod +x install.sh
./install.sh
```

Or build manually:

```bash
cd thirdParty && bash scripts/setup_glew && cd ..
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The binary will be at `build/bin/deskwaifu`.

The Qt control window will be at `build/bin/deskwaifu-control`.

To install a source build system-wide (default prefix: `/usr/local`):

```bash
sudo cmake --install build --component DeskWaifu
```

The component selects only the application commands and desktop entry, excluding
third-party development files. `install.sh` itself only builds the project.

## Upgrading from Waifuland

The DeskWaifu rename changes user-facing paths and identifiers. Existing data is
not migrated automatically:

- Move the contents of `$XDG_CONFIG_HOME/waifuland/` (or
  `~/.config/waifuland/`) to the corresponding `deskwaifu/` directory to keep
  your `config.json` and downloaded models.
- Update scripts and launchers to use `deskwaifu`, `deskwaifu-control`, and
  `deskwaifu-ctl`. The IPC socket is now `$XDG_RUNTIME_DIR/deskwaifu.sock`.
- Update compositor rules that match the layer-shell namespace from
  `waifuland` to `deskwaifu`.
- Update key bindings such as `killall -s SIGUSR1 waifuland` to target
  `deskwaifu`; current Hyprland examples are shown below.
- Stop any running Waifuland instance and remove its stale
  `$XDG_RUNTIME_DIR/waifuland.sock` (or `/tmp/waifuland.sock`) before starting
  DeskWaifu. After confirming the migration, remove the old `waifuland/`
  configuration directory.

## Usage

```bash
# Run with default model directory (~/.config/deskwaifu/models/)
./build/bin/deskwaifu

# Run with a custom model directory
./build/bin/deskwaifu --models_dir /path/to/your/models

# Run with a custom config file
./build/bin/deskwaifu --config /path/to/config.json

# Open the Qt control window after starting DeskWaifu
./build/bin/deskwaifu-control
```

The control window exposes the same model, expression, motion, lip-sync,
transform, visibility, skin, and look-at commands as `deskwaifu-ctl`. Its page
layout is stored in `control/MainWindow.ui` and can be edited with Qt Designer.

### Configuration

DeskWaifu reads a JSON config file from `$XDG_CONFIG_HOME/deskwaifu/config.json` (fallback: `~/.config/deskwaifu/config.json`). Override the path with `--config <path>`.

Example `config.json`:

```json
{
    "additional_model_dirs": [
        "/home/user/extra-models",
        "/opt/shared-models"
    ],
    "default_model": "MyFavoriteModel",
    "emotion_timeout": 5,
    "model_scale": 1.0,
    "model_x": 0.0,
    "model_y": 0.0,
    "window_width": 1900,
    "window_height": 1000
}
```

| Key | Type | Default | Description |
|---|---|---|---|
| `additional_model_dirs` | string[] | `[]` | Extra directories to scan for models (in addition to the default models dir) |
| `default_model` | string | `""` | Name of the model subfolder to load first on startup |
| `emotion_timeout` | float | `5` | Seconds before expression reverts to default. Set to `-1` to never revert |
| `model_scale` | float | `1.0` | Initial model scale |
| `model_x` | float | `0.0` | Initial model X offset |
| `model_y` | float | `0.0` | Initial model Y offset |
| `window_width` | int | `1900` | Render target width in pixels |
| `window_height` | int | `1000` | Render target height in pixels |
| `model_region` | object | Entire current output | Optional clipping rectangle: integer `x`, `y` (default `0`), and required positive integer `width`, `height` |

All fields are optional. Missing fields use their default values. If the config file doesn't exist, all defaults are used.

To clip the model to a rectangle, add this entry to your configuration:

```json
"model_region": { "x": 100, "y": 100, "width": 800, "height": 700 }
```

Coordinates are local to the current output's surface, measured from its top-left
corner (not global desktop coordinates). The rectangle is intersected with the
current surface on every frame, including after output switches or resizes.
Anything outside it is transparent and click-through; the model's position,
dragging and zoom are unchanged. A rectangle entirely outside the output makes
the model invisible and click-through. Omitting `model_region` preserves full-output
rendering: `x = 0`, `y = 0`, with the current output's width and height.
Malformed rectangles reject the configuration file with a diagnostic.

### Editing position, zoom and crop in the control panel

The **Transform** tab edits model position, zoom and the crop rectangle together.
Crop coordinates are integers in the current surface's top-left coordinate system;
they are not the model's floating-point offsets. **Enable crop** explicitly selects
clipping; a disabled crop uses the entire current surface without a fixed rectangle.

- Numeric edits create a preview. **Apply and save** commits all four configuration
  keys (`model_x`, `model_y`, `model_scale`, `model_region`) in one atomic update.
- **Drag edit** draws an on-screen frame. Its top-center handle moves **both the
  crop and the model by the same screen distance**. Edges/corners resize the crop
  without moving or scaling the model. Dragging the model itself moves only the
  model; scrolling changes its zoom. Model tap/hotkey actions are disabled while editing.
- **Cancel** or a disconnected control panel discards the preview. Output changes
  or resizes cancel screen editing. **Move back to screen** recovers offscreen crops.
- **Reset** immediately saves position `(0, 0)`, zoom `1` and no crop by removing
  those four keys; all other configuration is retained.
- Outside editing, a model drag saves on release, and scrolling saves after a
  short idle interval. Failed saves keep the on-screen result and offer **Retry
  save**. Automatic cross-output adaptation itself is not saved.

Only the model process writes configuration, using the actual `--config` path
(or the default path). Existing unrelated settings and comments outside replaced
values are preserved, as are file permissions and existing symbolic links. A
missing configuration is created; invalid documents and duplicate keys are never
silently overwritten. Files are checked every 0.5 seconds, so external editor
replacements are detected too. Only transform/crop settings are hot-reloaded;
other settings still require restarting the application.

External changes during preview do not replace the draft. Applying reports a
conflict: explicitly overwrite just the four transform/crop keys, or discard the
draft and reload. Only one connection can edit at a time. Save requests have IDs
whose recent outcomes can be queried after reconnecting; outcomes are not retained
across a backend restart. External editors do not honor the application's save
protocol: a content check immediately before rename narrows, but cannot completely
eliminate, concurrent external-write races. Requested crop rectangles are saved,
never their intersections with a smaller output.

### Controls

| Input | Action |
|---|---|
| **Left-click Upper Body** | Trigger expression change (if any) |
| **Left-click Lower Body** | Trigger motion (if any) | 
| **Left-click drag** | Drag the model / trigger hit areas |
| **Right-click** | Switch to next model |
| **Middle-click** | Switch skin (if any) |
| **Scroll wheel** | Zoom in/out |

### Model hotkeys in the control panel

The **Hotkeys** tab lists all entries from the current model's matching
`.vtube.json` file (for example, `桃小奈.model3.json` and `桃小奈.vtube.json`).
Each row shows the name, keys, action, current state and a **Toggle** button.
Empty names fall back to the expression filename. Missing files, disabled
entries, unsupported actions/keys and conflicting bindings are displayed in the list.

- Keys work across the control panel's tabs while its window is active. There
  are no global keyboard bindings, even for entries marked `IsGlobal` in VTube Studio.
- **Tab and Shift+Tab never move keyboard focus in the panel.** Tab executes a
  matching model binding, or does nothing when unbound. Holding a key does not
  repeatedly toggle the expression.
- Other keys retain their normal behavior while editing text, numbers or combo boxes.
- `ToggleExpression` supports independent persistent switches, fade transitions
  and optional timed deactivation. Turning off one switch does not clear others,
  and the ordinary `emotion_timeout` does not clear persistent switches.
- Expression files are resolved from the configured folder, the model root,
  `Expressions/`, or `exp/`. Persistent effects are blended in configuration order
  after ordinary expressions, using their `Add`, `Multiply` or `Overwrite` mode.
- Release-to-deactivate bindings and actions other than `ToggleExpression` are
  currently shown as unsupported. Buttons remain usable when only the keyboard
  mapping is unsupported or conflicting.
- Loading a different model refreshes the list and resets expression switches.

For example, 桃小奈's **Tab → 水印** binding toggles the watermark expression;
pressing Tab again removes that expression's contribution.

### Compositor Compatibility

DeskWaifu runs on any Wayland compositor that supports the `wlr-layer-shell` protocol. Some features require compositor-specific IPC and are only available on Hyprland:

| Feature | Hyprland | Sway | Other wlroots |
|---|---|---|---|
| Overlay rendering | Yes | Yes | Yes |
| Click-through transparency | Yes | Yes | Yes |
| Mouse drag / tap / scroll | Yes | Yes | Yes |
| Global cursor tracking (eyes follow cursor anywhere) | Yes | No | No |
| Cross-monitor drag | Yes | No | No |
| Move to focused monitor | Yes | Yes | No |

The compositor is auto-detected at startup. Feature availability is logged to the console.

### Toggling Visibility

You can hide and show your Live2D model (along with its click-through input region) by sending a `SIGUSR1` signal to the background process. When hidden, it uses virtually no resources.

```bash
kill -SIGUSR1 $(pgrep -x deskwaifu)
# Or
killall -s SIGUSR1 deskwaifu
# Or using the included toggle command:
./build/bin/deskwaifu toggle
```

### Move to Focused Monitor

Move the model to whichever monitor currently has focus by sending a `SIGUSR2` signal. This works on Hyprland and Sway.

```bash
kill -SIGUSR2 $(pgrep -x deskwaifu)
# Or
killall -s SIGUSR2 deskwaifu
# Or using the included focus command:
./build/bin/deskwaifu focus
```

**Hyprland keybind examples**  
Add these to your `~/.config/hypr/hyprland.conf`:
```ini
# Toggle deskwaifu visibility with Super + W
bind = SUPER, W, exec, killall -s SIGUSR1 deskwaifu
# Move deskwaifu to focused monitor with Super + Shift + W
bind = SUPER SHIFT, W, exec, killall -s SIGUSR2 deskwaifu
```

## IPC API

DeskWaifu exposes a Unix domain socket for external control by scripts and programs.

- **Socket path:** `$XDG_RUNTIME_DIR/deskwaifu.sock` (fallback: `/tmp/deskwaifu.sock`)
- **Protocol:** Newline-delimited JSON — send `{"command":"<name>", ...}\n`, receive `{"ok":true, ...}\n`
- **Requires:** `socat` (install via your package manager)

### CLI Client

A convenience CLI client `deskwaifu-ctl` is included in the project root:

```bash
./deskwaifu-ctl <command> [--key value ...]
```

### Available Commands

#### `get_hotkeys` / `trigger_hotkey` — Model hotkeys

`get_hotkeys` returns `hotkeys`, a `model_token`, and an optional `warning`.
Each entry contains `id`, `name`, `file`, `action`, `keys`, `enabled`, `active`
and `reason` (empty when the backend supports it).

```bash
./deskwaifu-ctl get_hotkeys
./deskwaifu-ctl trigger_hotkey --id <HotkeyID> --model_token <model_token>
```

`trigger_hotkey` returns the updated list. Use the token from the latest query;
stale tokens, unknown IDs and unavailable actions return an error. Toggles must
not be retried automatically if the connection drops before acknowledgement.

#### `get_status` — Get general status

Returns current model, zoom, position, and visibility.

```bash
./deskwaifu-ctl get_status
```

```json
{
    "ok": true,
    "model": "MyModel",
    "model_index": 0,
    "model_count": 3,
    "hidden": false,
    "zoom": 1.0,
    "x": 0.0,
    "y": 0.0
}
```

#### `get_available_models` — List all discovered models

```bash
./deskwaifu-ctl get_available_models
```

```json
{
    "ok": true,
    "models": ["MyModel", "AnotherModel", "ThirdModel"]
}
```

#### `get_current_model` — Get current model name and index

```bash
./deskwaifu-ctl get_current_model
```

```json
{
    "ok": true,
    "model": "MyModel",
    "index": 0
}
```

#### `set_model` — Switch model by name or index

```bash
# By name
./deskwaifu-ctl set_model --name "AnotherModel"

# By index
./deskwaifu-ctl set_model --index 2
```

```json
{
    "ok": true
}
```

#### `next_model` — Switch to next model

```bash
./deskwaifu-ctl next_model
```

```json
{
    "ok": true
}
```

#### `prev_model` — Switch to previous model

```bash
./deskwaifu-ctl prev_model
```

```json
{
    "ok": true
}
```

#### `get_expressions` — List expressions for current model

```bash
./deskwaifu-ctl get_expressions
```

```json
{
    "ok": true,
    "expressions": ["happy.exp3.json", "angry.exp3.json", "sad.exp3.json"]
}
```

#### `set_expression` — Set expression by ID

```bash
./deskwaifu-ctl set_expression --id "happy.exp3.json"
```

```json
{
    "ok": true
}
```

#### `get_motions` — List motions for current model

```bash
./deskwaifu-ctl get_motions
```

```json
{
    "ok": true,
    "motions": [
        {"group": "Idle", "index": 0, "file": "idle_01.motion3.json"},
        {"group": "TapBody", "index": 0, "file": "tap_01.motion3.json"}
    ]
}
```

#### `do_motion` — Play a motion

```bash
# Play a specific motion by group and index
./deskwaifu-ctl do_motion --group "TapBody" --index 0

# Play with custom priority (default: 2 = Normal)
./deskwaifu-ctl do_motion --group "Idle" --index 0 --priority 3

# Play a random TapBody motion (omit group)
./deskwaifu-ctl do_motion
```

```json
{
    "ok": true
}
```

#### `set_mouth_y` — Set mouth opening for external lipsync

Value range: `0.0` (closed) to `1.0` (fully open). Existing one-shot CLI
commands remain **persistent** across socket disconnects. Send continuously
for real-time lipsync using a single long-lived socket with
`"stream_source":true` (see below).

```bash
./deskwaifu-ctl set_mouth_y --value 0.8
./deskwaifu-ctl set_mouth_y --reset    # release the persistent mouth override
```

```json
{
    "ok": true
}
```

#### `set_model_zoom` — Set model zoom/scale

Value range: `0.1` to `10.0`.

```bash
./deskwaifu-ctl set_model_zoom --value 1.5
```

```json
{
    "ok": true
}
```

#### `set_model_position` — Set model position offset

```bash
./deskwaifu-ctl set_model_position --x 0.5 --y -0.3
```

```json
{
    "ok": true
}
```

#### `get_model_position` — Get current position and zoom

```bash
./deskwaifu-ctl get_model_position
```

```json
{
    "ok": true,
    "x": 0.5,
    "y": -0.3,
    "zoom": 1.5
}
```

#### `toggle_hidden` — Toggle window visibility

```bash
./deskwaifu-ctl toggle_hidden
```

```json
{
    "ok": true,
    "hidden": true
}
```

#### `switch_skin` — Switch model skin/outfit

```bash
./deskwaifu-ctl switch_skin
```

```json
{
    "ok": true
}
```

#### `set_look` — Override look-at direction (for head/face tracking)

```bash
# Set look direction (x, y range: -1.0 to 1.0)
./deskwaifu-ctl set_look --x 0.5 --y 0.3

# Reset to default (follow cursor)
./deskwaifu-ctl set_look --reset
./deskwaifu-ctl release_source        # release both persistent CLI channels
```

```json
{
    "ok": true
}
```

### Raw Socket Usage

For real-time mouth or look tracking, add `"stream_source":true` to each
`set_mouth_y` / `set_look` JSON command on a **long-lived connection**. The
server assigns a unique, unforgeable source identity to that connection;
closing it releases only its mouth/look values. A one-command connection with
`stream_source` releases immediately on close. Commands without the flag use
the shared persistent `cli` source and retain the pre-existing one-shot CLI
behavior. `set_mouth_y --reset` and `set_look --reset` clear only their
respective channel, and `release_source` clears both. For stream commands,
reset/release affects only the current connection. A successful switch to a
**different** model clears all external sources; failed selection or selecting
the current model preserves them. For each channel independently, live stream
sources outrank persistent CLI values, and the latest update within a tier
wins. When a stream is released, the previous eligible source becomes active.
`get_control_status` reports effective per-frame `has_mouth`, `mouth`,
`has_look`, `look_x`, and `look_y` values for diagnostics.
`./deskwaifu-ctl quit` stops the app gracefully (releasing GPU objects while
their EGL context is current). Model texture leases release uploads on the
last borrower; cached models obey a 128 MiB soft texture budget (the active
model is always kept even if it alone exceeds the budget).

```json
{"command":"set_mouth_y","value":0.7,"stream_source":true}
```

You can also communicate directly with the socket without `deskwaifu-ctl`:

```bash
# Using socat
echo '{"command":"get_status"}' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/deskwaifu.sock

# Using Python
python3 -c "
import socket, json
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect('$XDG_RUNTIME_DIR/deskwaifu.sock')
sock.send(b'{\"command\":\"get_status\"}\n')
print(sock.recv(4096).decode())
sock.close()
"
```

## Model Setup

Place Live2D models in your models directory. Each model should be in its own subfolder containing a `.model3.json` file:

```
~/.config/deskwaifu/models/
├── MyModel/
│   ├── MyModel.model3.json
│   ├── MyModel.moc3
│   ├── textures/
│   └── motions/
└── AnotherModel/
    └── ...
```

The default models directory is `$XDG_CONFIG_HOME/deskwaifu/models/` (fallback: `~/.config/deskwaifu/models/`). Override it with the `--models_dir` flag.

## Architecture

```
deskwaifu/
├── src/                    # Application source code
│   ├── main.cpp            # Entry point, CLI argument parsing
│   ├── LAppConfig.*        # JSON config file reader (singleton)
│   ├── LAppWayland.*       # Wayland client setup (display, compositor, EGL, layer-shell)
│   ├── LAppWaylandRegion.* # Input region management (click-through transparency)
│   ├── LAppDelegate.*      # Main app controller, render loop, input handling
│   ├── LAppLive2DManager.* # Model lifecycle management
│   ├── LAppView.*          # View/projection matrices, rendering coordination
│   ├── LAppModel.*         # Individual Live2D model instance
│   ├── LAppIPC.*           # IPC socket server for external control
│   └── LAppDefine.*        # Global constants and configuration
├── protocol/               # Wayland protocol XML files
│   ├── xdg-shell.xml
│   └── wlr-layer-shell-unstable-v1.xml
├── thirdParty/             # Third-party dependencies (GLEW, stb)
├── cmake/                  # CMake helper scripts
└── CMakeLists.txt          # Build configuration
```

### Key design decisions

- **No GLFW windowing** — Wayland surfaces are created directly via `wl_compositor` and `zwlr_layer_shell_v1` for overlay behavior that GLFW cannot provide.
- **EGL rendering** — OpenGL context is managed through EGL, bound directly to the Wayland display.
- **Layer-shell overlay** — the application renders as a Wayland layer surface, sitting above normal windows.
- **Input region masking** — only the model's bounding area accepts input; the rest of the surface is fully transparent and click-through.

## Built With

- [Live2D Cubism SDK for Native](https://www.live2d.com/en/sdk/about/) — Live2D model rendering
- [GLEW](https://github.com/nigels-com/glew) — OpenGL extension loading
- [stb_image](https://github.com/nothings/stb) — Image loading
- [wlr-layer-shell](https://wayland.app/protocols/wlr-layer-shell-unstable-v1) — Wayland overlay protocol
- **Wayland** / **EGL** / **OpenGL** — Display and rendering stack

## License

This project includes code adapted from the Live2D Cubism SDK samples, which are subject to the [Live2D Open Software License](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html).

The Live2D Cubism SDK (Core library) is proprietary and must be downloaded separately. See [Live2D SDK License](https://www.live2d.com/en/sdk/license/).
