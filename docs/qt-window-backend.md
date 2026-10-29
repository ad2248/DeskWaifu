# Qt model window

Qt is the only model-window implementation. The native Wayland/EGL window backend
and backend-selection build option have been removed. Cubism model/session,
application service, editing and configuration belong to desktop and remain
separate from its window port. JSON IPC belongs to the independent IPC plugin.
The optional control panel remains a separate process.

## Build and run

Requires Qt **6.5+** (`Gui`, `OpenGL`, `WaylandClient`, `StateMachine`,
`Concurrent`, `DBus`), the Qt Wayland platform plugin,
LayerShellQt **6.6+**, desktop OpenGL **3.3 compatibility profile**, EGL, libsystemd
development files and the existing Cubism/GLEW dependencies. The default control
panel build also requires Qt `Widgets` and `Network`. The speech plugin is enabled
by default and adds `WebSockets` and `Multimedia`; disabling it with
`DESKWAIFU_BUILD_SPEECH_PLUGIN=OFF` removes those requirements, but is not supported
by the complete regression gate. With `BUILD_TESTING=ON`
(the default), the full default test set additionally requires Qt `Network`/`Test`,
`wayland-scanner` and wayland-client development files. On Arch the LayerShellQt
package is `layer-shell-qt`; other distributions may split its library and
development headers into packages.

Desktop, perception, IPC, speech/reading capabilities and control-panel tabs are loaded from
plugins. For development, point backend `plugin_dir` and control-panel
`DESKWAIFU_CONTROL_PLUGIN_DIR` at the build's `plugins` directory; their defaults
refer to the install prefix, not the build tree. See
[plugin architecture](plugin-architecture.md) and [GUI plugins](gui-plugins.md).

```sh
cmake -S . -B build-qt
cmake --build build-qt
QT_QPA_PLATFORM=wayland build-qt/bin/deskwaifu --models_dir /path/to/models
```

`SDK_ROOT_PATH` and `GLEW_PATH` can point to existing dependency source directories
when working in an independent worktree. Qt remains required when
`DESKWAIFU_BUILD_CONTROL=OFF`; this switch only disables the control panel.

The executable is still named `deskwaifu`; the CLI, IPC socket and config paths
are unchanged. Do not run multiple model processes against the same runtime socket.
The desktop plugin rejects non-Wayland platforms or compositors without layer-shell
instead of silently presenting an ordinary desktop window. The platform and other
healthy plugins can remain active when desktop fails to initialize.
With the required Qt 6.5+, `LayerShellQt::Window::get()` selects the shell
integration per window; the old application-wide `Shell::useLayerShell()` call
is no longer required. The isolated smoke test removes shell integration
environment overrides and asserts actual `get_layer_surface` requests, with
no `get_xdg_surface` fallback, for both the window fixture and real model.

## Boundaries and behavior

- `DesktopWindow` is the application's window/output/input port, implemented by
  `QtDesktopWindow` using `QWindow`, `QOpenGLContext` and LayerShellQt. Qt owns its
  connection and surface: the application never dispatches Qt's Wayland socket
  or manages Qt's EGL handles itself.
- A GUI-thread Qt timer drives window presentation at 16 ms intervals, slowing to
  33 ms when the input region is empty. The independent model-render timer stays
  at 16 ms; empty input does not slow model rendering. IPC keeps its Unix socket
  and JSON protocol, but `QSocketNotifier` now drives readiness-based dispatch,
  paused during output replacement. Config polling remains presentation-driven.
  `IDesktopWindowEvents` carries input without exposing native protocol objects.
- The GL compatibility profile is necessary because the current Cubism/sprite
  renderer uses client-side vertex/index arrays. A core-only or OpenGL ES context
  is rejected. The integration test catches accidental core-context creation.
- Model rendering runs on a dedicated thread with a separate offscreen context
  and nonzero FBO. The default/`auto` render target uses `QOffscreenSurface` and
  `QOpenGLContext`; selecting a PCI render device uses an application-managed EGL
  context, not a native window backend. Frames are read back to CPU RGBA memory
  and uploaded to the GUI thread's presentation FBO; this is not a shared-texture
  or zero-copy path. Qt scales only final presentation to the backing buffer.
  Crop rectangles, input masks and mouse events remain in **logical
  output-local pixels**. This first version renders at logical resolution even
  on HiDPI displays: a higher-resolution model render target is future work.
- Input masks are derived from the model's own alpha before hover opacity and
  editor decorations. An empty silhouette explicitly sets
  `Qt::WindowTransparentForInput`: an empty `QWindow::setMask()` alone means reset
  to whole-window input. Normal masks restore input; screen editing accepts the
  entire output, as before.
- Qt double-click events are forwarded as presses so the existing click policy
  remains authoritative. Native Wayland XKB scan codes are translated back to
  evdev codes. Auto-repeat is ignored; focus loss releases the layer keyboard
  interactivity and clears held keys. No global shortcuts are introduced.
  Double-click requests layer-shell keyboard interactivity without consulting
  `QInputDevice::devices()`: Qt may not list a keyboard until the first key event,
  so that list cannot gate the focus request needed to receive keyboard input.
- `QScreen` provides logical output geometry and stable IDs are assigned for the
  lifetime of each screen. A screen replacement reuses the window's GL context;
  input callbacks are suppressed while the replacement is being configured.
  Removing the current screen falls back to another screen; re-added screens
  receive fresh IDs. Model resources are released on the render thread with its
  context current, before window context/surface teardown.
- Global cursor gaze still uses the existing KWin bridge. Focused-output and
  cross-output drag capabilities retain their compositor-specific limitations.
  Qt does not grant global-pointer access on Wayland. Sway/Hyprland behavior still
  needs physical-pointer and visual-quality checks on real desktops.

## Validation

```sh
ctest --test-dir build-qt --output-on-failure
python3 tests/qt_headless_smoke.py build-qt /path/to/CubismSdkForNative-5-r.5
python3 tests/qt_headless_smoke.py build-qt /path/to/CubismSdkForNative-5-r.5 --scale 1.5
```

The smoke runner requires a build with `BUILD_TESTING=ON`, Python 3,
`kwin_wayland` with virtual-output and fake-input v3 support, `dbus-run-session`,
and the SDK's `Samples/Resources/Haru` model. It creates a private D-Bus session,
two virtual outputs, runtime socket and config directory, with software rendering.
`kscreen-doctor` is required for any non-1 scale (including `--scale 1.5`) to apply
actual output scaling. When installed, it also disables and re-enables an output
to exercise hotplug. Input injection is confined to the private compositor; no
input is sent to the user's desktop. Logs remain in
`BUILD_DIR/qt-smoke-logs/SCALE/`.

The CI Sway smoke runner deliberately has no input devices. It passes
`--no-keyboard` to the window fixture and verifies the seat's missing keyboard
capability in the protocol trace; focus requests must still succeed locally.
The KWin smoke runner requires a keyboard-capable seat and actual window focus.

The window integration executable verifies GL color/alpha in both the model FBO
and the actual window back buffer immediately before EGL swap. It deliberately
sets the window draw target to `GL_NONE` before presentation to cover the NVIDIA
invisible-window regression, including output replacement and fractional scale.
It also verifies logical input masks,
empty-input restoration, synthetic Qt mouse/key dispatch, keyboard focus loss,
output switching and ordered teardown. The runner additionally tests cross-context
RGBA frame transfer and actual compositor click routing through empty, shaped and
restored input regions, checks Wayland input-region requests, and runs a real SDK
model through motion, expression, mouth/look, transform/crop saves, screen-edit,
visibility, model switch and quit IPC. This does **not** replace manual checks of
physical pointer click-through, compositor focus policy or visual quality on real
desktops.

The model currently emits the SDK's existing
`GL_INVALID_VALUE in glDeleteProgram` teardown diagnostic (shader sets share
program IDs). The smoke test allows only that exact diagnostic **after quit**;
OpenGL errors during rendering still fail the test. No third-party SDK source
is modified as part of the backend migration.

The optional `DESKWAIFU_QT_LIVE_TEST=ON` CMake switch registers the window test
with CTest against the selected display. Use it only with an isolated compositor,
never against the user's active session.
