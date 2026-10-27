# DeskWaifu

> **Disclaimer:** This repository is "vibe coded". Please use with caution.

A Linux desktop Live2D model viewer that renders Live2D characters as transparent Wayland overlay windows using the `wlr-layer-shell` protocol.

Live2D models float on your desktop with click-through transparency — only the model itself receives input. Built with the Live2D Cubism SDK for Native, OpenGL, Qt 6 and LayerShellQt. Qt manages the Wayland window and its graphics context; there is no separate native window backend.

The legacy GLFW minimum demo has been retired; the Qt application is the only supported entry point.

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
- **Multi-output support** — choose and remember a monitor from the control panel
- **Interactive** — responds to mouse drag, tap, and scroll input
- **Optional speech plugin** — double-click the model to say “姐姐，你好喵” through MiniMax's bidirectional WebSocket API; another double-click interrupts and restarts speech
- **Motion & expression** — supports idle animations, lip-sync, eye-blink, physics, and expressions
- **Configurable model directory** — load models from any path via CLI flag or XDG config
- **JSON configuration** — customize behavior via `config.json` (default model, emotion timeout, additional model dirs, scale/position, window size)
- **IPC control** — Unix domain socket API for external control (model switching, expressions, motions, lipsync, zoom, position, look-at direction)
- **Optional local OCR (experimental)** — an independently installed C++/ONNX Runtime helper owned by the perception plugin, without Python or OCR engine dependencies in the platform. See [OCR package and protocol](src/plugins/perception/native/README.md).

## Prerequisites

| Dependency | Notes |
|---|---|
| **Linux with Wayland compositor** | Must support `wlr-layer-shell-v1`; KDE Plasma 6 / KWin is required for global cursor tracking |
| **Live2D Cubism SDK for Native** | Download from [live2d.com/sdk](https://www.live2d.com/en/sdk/about/) (proprietary, not bundled) |
| **C++ compiler** | GCC or Clang with C++17 support for Qt 6 |
| **CMake** | >= 3.16 |
| **pkg-config** | For finding Wayland/EGL libraries |
| **Wayland development libraries** | Required by Qt Wayland; Qt owns protocol dispatch |
| **libsystemd development library** | `sd-bus` for the transient KWin cursor bridge |
| **EGL & OpenGL** | `libegl-dev`, `libgl-dev` or equivalent |
| **Qt 6.5+** | `Gui`, `OpenGL`, `WaylandClient`, `StateMachine`, `WebSockets`, `Multimedia`, `DBus`, `Concurrent` and the Qt Wayland platform plugin; `Widgets` and `Network` for the optional control panel |
| **LayerShellQt 6.6+** | Required for the model's desktop overlay window |
| **curl, unzip** | For downloading third-party dependencies (GLEW) |

### Installing system dependencies

**Arch Linux:**
```bash
sudo pacman -S --needed base-devel cmake pkgconf wayland wayland-protocols libglvnd egl-wayland qt6-base qt6-wayland qt6-scxml qt6-websockets qt6-multimedia layer-shell-qt systemd-libs curl unzip
```

**Ubuntu / Debian:**
```bash
sudo apt install build-essential cmake pkg-config libwayland-dev wayland-protocols libegl-dev libgl-dev libglu1-mesa-dev libsystemd-dev qt6-base-dev libqt6opengl6-dev qt6-wayland qt6-wayland-dev qt6-scxml-dev qt6-websockets-dev qt6-multimedia-dev curl unzip
```

**Fedora:**
```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config wayland-devel wayland-protocols-devel mesa-libEGL-devel mesa-libGL-devel systemd-devel qt6-qtbase-devel qt6-qtwayland-devel qt6-qtscxml-devel qt6-qtwebsockets-devel qt6-qtmultimedia-devel layer-shell-qt-devel curl unzip
```

Install LayerShellQt **6.6+** development files as well. On distributions shipping
older Qt/Plasma packages, a newer package repository or source build may be needed.

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

The model window always uses Qt and LayerShellQt; no backend-selection flag is
needed. Disabling `DESKWAIFU_BUILD_CONTROL` only omits the control panel, not the
Qt model window. See [window behavior and validation](docs/qt-window-backend.md).
Lifecycle, dragging, and crop editing are controlled by a
[hierarchical Qt state machine](docs/model-window-state-machine.md).

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

### GUI plugins

The control panel loads its system, interaction, model and log tabs from GUI
plugins. A plugin may export a backend entry, a GUI entry, or both from the same
`.so`; the speech plugin provides both. See [GUI plugin architecture](docs/gui-plugins.md).
The control panel uses `plugin_dir` from the main configuration. For a build-tree
run, set `DESKWAIFU_CONTROL_PLUGIN_DIR=/absolute/path/to/build/plugins`.

### Streaming speech

Speech is provided by an optional dynamically loaded plugin. The host discovers
`<plugin_dir>/<name>/<name>.so` on startup, without a manifest. Set `plugin_dir` in
the main configuration to use a development or custom installation directory.
See [plugin boundaries and migration](docs/plugin-architecture.md).

With the speech plugin installed, enter and save a MiniMax API Key in the control
panel's **语音** tab. The plugin stores its configuration under
`plugin/speech/` next to the main configuration and restricts
the credentials file to owner read/write (`0600`). The key is stored in plaintext, not encrypted;
do not share or commit this configuration. The GUI only queries whether a key is
configured, never its value. Use the delete button to remove it.
Keys in the old main configuration are not automatically imported: re-enter the
key through the control panel, then remove the old `minimax_api_key` field yourself.

Double-click the model to stream “姐姐，你好喵”. This retains the existing keyboard
focus behavior; another double-click cancels the previous speech and starts again.
Speech uses `speech-2.8-turbo`, `female-tianmei`, and 24 kHz mono PCM through the
MiniMax bidirectional WebSocket API. Text leaves your device and API usage may be
charged. Missing credentials or unavailable audio devices do not prevent normal
model interaction. Crop/edit-mode double-clicks do not trigger speech.

Speech tests normally use a local fake server and require no credentials. Opt-in
real API and audio-device tests can be run separately:

```bash
DESKWAIFU_TTS_LIVE_KEY_FILE=/private/MINIMAX.env build/bin/MiniMaxSpeechSource-test liveSynthesis
DESKWAIFU_AUDIO_TEST=1 build/bin/AudioPlayer-test
```

The test-only env file contains `API_KEY=...`; the application itself uses the GUI
configuration, not this file. Audio-device tests play silence unless both variables
are set, which enables `liveSpeechPipeline` to synthesize and play the greeting.
Rebuild and privately
transfer the CI image after adding the new Qt dependencies, using
`scripts/rebuild-ci-image.sh`, then update the runner's image-ID pin as it instructs.

```bash
# Run with default model directory (~/.config/deskwaifu/models/)
./build/bin/deskwaifu

# Run with a custom model directory
./build/bin/deskwaifu --models_dir /path/to/your/models

# Run with a custom config file
./build/bin/deskwaifu --config /path/to/config.json

# Open the Qt control window (or start DeskWaifu from its Spawn button)
./build/bin/deskwaifu-control
```

### Render on a different GPU (Linux)

To keep model textures, masks and render targets off the display GPU, select the
render GPU by its lowercase PCI address (`lspci -D`). For example, render on an
Intel iGPU while leaving the Qt Wayland window on the compositor-selected GPU:

```bash
./build/bin/deskwaifu --render-device 0000:00:02.0
```

Alternatively, persist `"render_device": "0000:00:02.0"` in the JSON config, or
set `DESKWAIFU_RENDER_DEVICE=0000:00:02.0`. Precedence is CLI, environment, config.
`auto` (the default) preserves the original single-context path. Restart the app
after changing the selection.

The control window's **Overview → 渲染设备 → 模型渲染 GPU** dropdown defaults to
**自动** and lists the backend's EGL hardware devices by name and PCI address.
Connect to a backend to change it: selection saves to that backend's actual
config file (including a custom `--config` path), then **Respawn** applies it.
The running renderer is not changed in place. Save errors restore the confirmed
selection; missing saved devices are shown as unavailable rather than silently
replaced with automatic selection. CLI/environment overrides still take priority.

IPC `get_render_device` returns the saved `device`, the `active_device` startup
selection, `restart_required`, and a `devices` array of `{id, name}` objects.
`set_render_device` accepts `{"device":"auto"}` or an available device's PCI
address and persists the next-start preference without switching the live context.

The selected GPU uses a separate device-platform EGL/OpenGL 3.3 compatibility
context and needs no connected monitor. Only the final premultiplied RGBA frame
is read into CPU memory and uploaded to the Qt window's GPU. This reduces model
VRAM residency on the display GPU, but does not eliminate presentation VRAM and
adds CPU transfer/bandwidth cost, especially at high resolutions. This is not a
DMA-BUF/zero-copy path.

Remove the earlier experimental `DRI_PRIME`, `__EGL_VENDOR_LIBRARY_FILENAMES`
and `__EGL_VENDOR_LIBRARY_DIRS` overrides before using explicit device selection;
they can redirect the window GPU or hide devices from EGL enumeration. The app
reports the window renderer and selected model renderer separately. An invalid
or unavailable requested device fails startup rather than silently falling back.

Hardware checks (the window test requires an isolated layer-shell compositor):

```bash
DESKWAIFU_TEST_RENDER_DEVICE=0000:00:02.0 ./build/bin/offscreen-renderer-test
DESKWAIFU_TEST_RENDER_DEVICE=0000:00:02.0 ./build/bin/gpu-transfer-test
```

The transfer test checks premultiplied color, transparency and orientation in the
actual window back buffer before swapping, and switches outputs when available.

### Control window

The control window exposes the same model, expression, motion, lip-sync,
transform, visibility, skin, and look-at commands as `deskwaifu-ctl`. Its
shell layout is stored in `platform/control/MainWindow.ui`; plugin pages live in
`src/plugins/<name>/gui/` and can be edited with Qt Designer.

The five top-level tabs are:

- **系统**: status, process controls and rendering GPU settings.
- **交互**: model hotkeys, cursor tracking, look-at overrides and opacity behavior.
- **模型**: model selection, visibility, skins, expressions, motions, lip sync,
  target monitor, position, zoom and crop.
- **语音**: speech provider and credential settings.
- **日志**: explicitly published, credential-safe diagnostics (not raw IPC responses).

Settings pages scroll vertically on smaller windows; connection and operation
feedback remain in the shared status bar.

In **系统 → 进程控制**, **启动** starts `deskwaifu` only when the
control window is disconnected and no `deskwaifu` process owned by the current
user is found. It looks beside the control executable first, then in `PATH`,
and launches with the default backend settings. Existing processes are left
alone while the control window retries connecting. The backend runs detached:
closing the control window does not stop it. Startup errors appear in the status
bar and response log; the control window retries connections every 1.5 seconds.
Concurrent Spawn requests share a per-user runtime lock around process inspection
and startup, so two control windows cannot both start a backend at the same time.
When connected or a running backend is detected, the button becomes **Shutdown**.
It sends `quit` once connected; while waiting for a connection it is disabled.
After the backend exits, the button returns to **Spawn**.

**Respawn** is available while connected. It sends `quit`, waits for the old
process to exit, then uses the same detached startup and automatic reconnection
as Spawn. Spawn and Respawn are disabled during this operation. If shutdown is
rejected or takes more than 10 seconds, the GUI reports an error without forcing
termination or starting an overlapping backend. Like Spawn, Respawn starts with
default backend settings; custom command-line arguments (including `--config`)
and unsaved runtime changes are not preserved.

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
| `render_device` | string | `"auto"` | Model rendering GPU PCI address, e.g. `0000:00:02.0`; restart required. `auto` keeps the original window GPU. |
| `emotion_timeout` | float | `5` | Seconds before expression reverts to default. Set to `-1` to never revert |
| `model_scale` | float | `1.0` | Initial model scale |
| `model_x` | float | `0.0` | Initial model X offset |
| `model_y` | float | `0.0` | Initial model Y offset |
| `window_width` | int | `1900` | Render target width in pixels |
| `window_height` | int | `1000` | Render target height in pixels |
| `hover_opacity_enabled` | bool | `false` | Reduce model opacity while the pointer is over its input silhouette |
| `hover_opacity` | float | `0.5` | Hover opacity, from `0` (invisible) to `1` (unchanged); must be finite |
| `idle_opacity` | float | `1.0` | Normal model opacity when not hovered, or when hover opacity is disabled; finite number within `0..1` |
| `mouse_look_radius` | float | `0.5` | Maximum normalized mouse gaze amplitude; finite non-negative float. `0` keeps mouse gaze centered; external look controls are unaffected |
| `model_region` | object | Entire current output | Optional clipping rectangle: integer `x`, `y` (default `0`), and required positive integer `width`, `height` |
| `model_output` | string | First enumerated output | Display connector name, e.g. `DP-1`; an unavailable or unset name is replaced with the first enumerated output and saved at startup |

All fields are optional. Missing fields use their default values. If the config file doesn't exist, all defaults are used.

To configure half-transparency on hover at startup, add these fields:

```json
"hover_opacity_enabled": true,
"hover_opacity": 0.5
```

Configuration keys use snake_case. The model returns to its normal opacity when
the pointer leaves. Hover opacity is applied after the model is rendered, preserving
its own material/animation alpha. It does not enable click-through: even at `0`,
the original model input silhouette remains active, so leaving restores visibility
without flicker. Transparent background and cropped-out areas stay click-through;
editor handles do not count as model hover. The silhouette follows the existing
10-pixel block sampling rather than an exact per-pixel hit test. Invalid hover
values reject the configuration with a diagnostic. The feature is disabled by
default, with no extra offscreen rendering while disabled.

The **交互** tab provides a **模型透明度** section. Toggle
**启用悬停半透明** to enable or disable the effect. Drag the opacity slider from
`0%` to `100%`: the percentage updates while dragging, and releasing applies and
saves the value immediately, without **Apply and save** or a restart. Keyboard
and mouse-wheel adjustments also apply directly. This setting is independent of
the position/crop edit transaction and its reset/cancel buttons. Failed saves
show an error and restore the effective backend value. Disconnected or older,
unsupported backends disable the controls; reconnecting queries the actual saved
state rather than replaying a write with an unknown outcome.

The bottom status bar keeps connection information on the left and operation
feedback (saving, success, or errors) on the right, visible across all tabs.
Successful background queries do not replace the latest operation message.
`MainWindow::setStatusMessage(message, timeoutMs)` updates this shared feedback;
an empty message clears it, and a positive timeout clears it automatically.

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

The **模型** tab edits model position, zoom and the crop rectangle together.
Its **显示器** selector immediately switches and saves the display. Switching centers
the model at `(0, 0)`, preserves zoom, and clears the crop. Finish or cancel an active
transform edit before switching. Restarting restores the selected display and saved
placement; an unavailable display falls back to the first enumerated display, centers
the model, clears the crop, and writes the replacement name to configuration.
If startup cannot save that fallback, it logs a warning and continues on the first
display using the readable configuration's placement instead of refusing to start.
Crop coordinates are integers in the current surface's top-left coordinate system;
they are not the model's floating-point offsets. **Enable crop** explicitly selects
clipping; a disabled crop uses the entire current surface without a fixed rectangle.

- Numeric edits create a preview. **Apply and save** updates the transform/crop
  fields and atomically serializes all current configuration values.
- **Drag edit** draws an on-screen frame. Its top-center handle moves **both the
  crop and the model by the same screen distance**. Edges/corners resize the crop
  without moving or scaling the model. Dragging the model itself moves only the
  model; scrolling changes its zoom. Model tap/hotkey actions are disabled while editing.
- **Cancel** or a disconnected control panel discards the preview. Output changes
  or resizes cancel screen editing. **Move back to screen** recovers offscreen crops.
- **Reset** immediately writes position `(0, 0)` and zoom `1`, omitting
  `model_region` to disable cropping; other known configuration values are retained.
- Dragging the model automatically enters screen editing once the drag threshold
  is crossed. Releasing keeps the crop frame visible. Dragging the model moves
  only the model; the top-center handle moves both model and crop. With no crop
  configured, editing starts with a full-surface crop.
- On Hyprland, releasing a locally dragged model on another monitor moves it to
  that monitor and keeps screen editing active. The crop starts at the destination's
  full extent; save it explicitly when ready. Frame handles and control-panel-owned
  edits do not switch monitors. Without global cursor support, dragging stays local.
- Double-click a crop frame edge or its top-center handle to apply and save, then
  hide the frame (the crop remains active). This also works for **Drag edit**
  started from the control panel. Failed saves keep the frame and preview; details
  appear in the control panel and application log. Double-click again to retry
  after resolving the error. Model taps and keyboard grabs are disabled in editing.
- Outside editing, scrolling saves after a short idle interval. Failed automatic
  saves offer **Retry save**. Automatic cross-output adaptation itself is not saved.

Only the model process writes configuration, using the actual `--config` path
(or the default path). Every save writes all known configuration fields, including
defaults, from the configuration value structure; internal metadata is excluded.
Comments and unknown fields are not preserved. File permissions and existing symbolic links are preserved. A
missing configuration is created; invalid documents and duplicate keys are never
silently overwritten. Files are checked every 0.5 seconds, so external editor
replacements are detected too. Transform/crop and hover settings are hot-reloaded.
Hover settings are also refreshed when queried by the control panel; other settings
still require restarting the application.

For IPC clients, `get_transform_settings` includes `outputs`, an array of
`{"name":"DP-1","current":true}` objects. `{"command":"set_output","name":"DP-1"}`
selects an available display and returns `ok` or an error; failed saves attempt to
restore the previous display. Selecting the already configured current display is a no-op.

External changes during preview do not replace the draft. Applying checks the entire
configuration for conflicts, including independent hover saves during preview:
explicitly overwrite the complete configuration, or discard the draft and reload.
Only one connection can edit at a time. Save requests have IDs
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

The **交互 → 模型快捷键** section lists all entries from the current model's matching
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

DeskWaifu runs on Wayland compositors that support `wlr-layer-shell`.
Global cursor tracking targets KDE Plasma 6 / KWin Wayland:

| Feature | KDE / KWin | Hyprland | Sway | Other wlroots |
|---|---|---|---|---|
| Overlay rendering | Yes | Yes | Yes | Yes |
| Click-through transparency | Yes | Yes | Yes | Yes |
| Mouse drag / tap / scroll | Yes | Yes | Yes | Yes |
| Global cursor tracking (eyes follow cursor anywhere) | Yes | No | No | No |
| Cross-monitor drag | No | Yes | No | No |
| Move to focused monitor | No | Yes | Yes | No |

On KDE, DeskWaifu loads a transient KWin script at startup through session D-Bus.
The script is read from an anonymous memory file: nothing is installed in KDE's
script list and no KDE configuration is changed. Before registering any script,
DeskWaifu starts an independent cleanup subprocess. It watches a control pipe:
normal shutdown or a crash closes that pipe, so it can unload even a registered
script that never started (for example, after a lost `loadScript` reply). Cleanup
targets the original KWin connection and retries transient failures. Running
scripts additionally detect the application's vanished D-Bus connection and
unload themselves (normally within two seconds). If KWin restarts, DeskWaifu
reloads the script and starts a cleaner for the new connection.

The script samples the global cursor and the model output's logical geometry at
most once every 30 ms, with at most one sample call in flight. This supports gaze
outside the model/crop region and on other monitors, including scaled outputs.
The latest sample is reapplied every visible frame, including for a stationary
cursor after taps or model transform changes. It does not synthesize model drag
or crop-edit events. If the KWin bridge is unavailable, or on another compositor,
gaze falls back to pointer events on the model surface.

For an opt-in real-KWin lifecycle smoke test (normal exit and SIGKILL cleanup), run
`build/bin/kwin-cursor-tracker-test --live DP-1`, replacing `DP-1` with the model
output's name. Automated tests use a private D-Bus session and a fake compositor;
they do not alter the running desktop.

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
- **CLI requires:** Python 3 (standard library only; install via your package manager).

### Transport limits

The server accepts at most 16 concurrent clients. Request lines may contain up
to 64 KiB and individual replies up to 8 MiB, excluding the terminating newline.
An oversized frame closes only that connection; excess connections are closed
without replacing existing clients. A lost reply does not prove that a command
was not executed: query the resulting state rather than replaying a mutation.

Replies are sent non-blockingly and in request order. Each connection retains
only one pending reply; while it is pending, that client's further commands are
not executed or read. This backpressure does not stop other clients. Every frame
permits at most 4 accept attempts and 16 request lines (4 per client), with the
first served client rotated. Each client reads at most 4 KiB and writes at most
64 KiB in up to 16 send attempts per frame. Empty lines and interrupted attempts
also consume their respective budgets. These bound transport work, not the cost
of executing an individual application command or generating its reply.

### CLI Client

A convenience CLI client `deskwaifu-ctl` is included in the project root:

```bash
./deskwaifu-ctl <command> [--key value ...]
```

The CLI serializes names/IDs as strings (including numeric-looking names such
as `001`) and numeric/boolean parameters by their protocol type. It exits with
status 0 only for a complete JSON response containing `"ok":true`. Backend
errors remain visible in the printed response and return status 1, as do
communication errors, a five-second socket timeout, and empty, malformed or
truncated responses. No `socat` dependency is needed for the CLI.

Raw requests must be a single valid JSON object per line, without comments or
duplicate keys. Only top-level fields participate in command dispatch. Names,
IDs and tokens must be strings; numeric fields must be JSON numbers, and integer
fields reject fractions, overflow and numeric strings. `reset` and
`stream_source` retain legacy boolean spellings: booleans, `0`/`1`, and the
strings `"false"`/`"true"`/`"0"`/`"1"`. Other boolean fields require JSON booleans.

### Available Commands

#### `get_mouse_look` / `set_mouse_look` — Mouse gaze range

```json
{"command":"get_mouse_look"}
{"command":"set_mouse_look","radius":0.5}
```

The radius limits the length of the normalized mouse gaze vector, preserving its
direction. Targets inside the circle are unchanged; targets outside it are clamped
to the circle, including global cursor samples on other displays. Smaller values
reduce eye and head movement. `0` centers mouse gaze; external `set_look` and stream
overrides are unaffected. Missing configuration values default to `0.5`.

The control window's **鼠标注视范围 / 最大半径** slider covers `0..2`, with a numeric
input for larger values (up to `10`). Changes save on release or numeric confirmation.
Both commands return `radius`; failed saves return `ok: false`, `error`, and the
unchanged effective value. Set requests require a finite non-negative float and
update runtime state only after an atomic save to the actual configuration path.
Known unrelated fields are preserved; comments and unknown fields are not.
Queries refresh from disk. After a lost acknowledgement, query instead of replaying
the set request.

#### `get_hover_opacity` / `set_hover_opacity` — Model opacity

```json
{"command":"get_hover_opacity"}
{"command":"set_hover_opacity","enabled":true}
{"command":"set_hover_opacity","opacity":0.5}
{"command":"set_hover_opacity","idle_opacity":0.8}
```

Both commands return `enabled`, `opacity` and `idle_opacity`, including the unchanged effective
values when a save fails (`ok: false`, with `error`). Setting requires at least
one of `enabled` (boolean), `opacity` or `idle_opacity` (finite numbers within `0..1`).
`enabled` controls only the hover override: otherwise `idle_opacity` applies.
The two opacities are absolute display multipliers, not multiplied together.
The control window provides independent normal and hover sliders; both save on release.
Even at zero opacity, the model retains its original input silhouette. Supplied
fields update a configuration value structure loaded from disk, which is then
serialized in full to the backend's actual configuration path. Other known fields
are retained; comments and unknown fields are not. Runtime values change only after a successful atomic
save. Queries refresh the current settings from disk. Do not automatically retry
a set request after losing its acknowledgement; query the current settings instead.

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
├── sdk/                    # Public plugin APIs, event runtime and shared contracts
├── platform/               # Process/GUI hosts, discovery, lifecycle and platform tests
├── src/plugins/            # Independently built business modules and their tests
│   ├── desktop/            # Model, rendering, window, interaction and input
│   ├── perception/         # Image/OCR/screenshot services; optional native OCR helper
│   ├── ipc/                # Backend socket transport, command gateway and GUI client
│   ├── speech/             # Speech providers, playback and speech settings page
│   ├── reading_clip/       # Clipboard reading orchestration
│   └── system|interaction|model|log/  # GUI-only modules
├── thirdParty/             # Third-party dependencies (GLEW, stb)
├── cmake/                  # CMake helper scripts
└── CMakeLists.txt          # Build configuration
```

### Key design decisions

- **Three ownership boundaries** — SDK, platform and plugins have separate build dependencies. Business implementations, including IPC transport, belong to plugins; the platform neither links nor directly assembles them. See [plugin architecture](docs/plugin-architecture.md).
- **Event processors** — plugin-local processors use queued events; cross-plugin command routing uses SDK capability requests with explicit responses, cancellation, timeout and provider errors. Cubism resources and rendering belong to the desktop plugin's dedicated render thread.
- **Qt-only windowing** — `QWindow` and LayerShellQt own the overlay window, input and screen lifecycle; the application does not manage raw Wayland surfaces.
- **OpenGL rendering** — Cubism draws into a Qt-managed context. Qt's Wayland platform plugin may use EGL internally; the application does not create or swap EGL window surfaces.
- **Layer-shell overlay** — the application renders as a Wayland layer surface, sitting above normal windows.
- **Input region masking** — only the model's bounding area accepts input; the rest of the surface is fully transparent and click-through.

## Built With

- [Live2D Cubism SDK for Native](https://www.live2d.com/en/sdk/about/) — Live2D model rendering
- [GLEW](https://github.com/nigels-com/glew) — OpenGL extension loading
- [stb_image](https://github.com/nothings/stb) — Image loading
- [wlr-layer-shell](https://wayland.app/protocols/wlr-layer-shell-unstable-v1) — Wayland overlay protocol
- [Qt 6](https://www.qt.io/) / [LayerShellQt](https://invent.kde.org/plasma/layer-shell-qt) — Model window and desktop overlay integration
- **Wayland** / **EGL** / **OpenGL** — Display and rendering stack

## License

This project includes code adapted from the Live2D Cubism SDK samples, which are subject to the [Live2D Open Software License](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html).

The Live2D Cubism SDK (Core library) is proprietary and must be downloaded separately. See [Live2D SDK License](https://www.live2d.com/en/sdk/license/).
