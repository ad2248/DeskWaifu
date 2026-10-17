# Experimental Qt model window backend

The Qt backend is opt-in. The default `wayland` backend is retained for comparison
and rollback; both run the same Cubism model/session, application service, editing,
configuration and JSON IPC code. The control panel remains a separate process.

## Build and run

Requires Qt **6.5+** (`Gui`, `OpenGL`, `WaylandClient`), the Qt Wayland platform
plugin, LayerShellQt **6**, desktop OpenGL **3.3 compatibility profile**, and the
existing Cubism/GLEW dependencies. On Arch the extra package is `layer-shell-qt`;
other distributions may split its library and development headers into packages.

```sh
cmake -S . -B build-qt -DDESKWAIFU_WINDOW_BACKEND=qt
cmake --build build-qt
QT_QPA_PLATFORM=wayland build-qt/bin/deskwaifu --models_dir /path/to/models
```

`SDK_ROOT_PATH` and `GLEW_PATH` can point to existing dependency source directories
when working in an independent worktree. Use another build directory with
`-DDESKWAIFU_WINDOW_BACKEND=wayland` to build the original backend.

The executable is still named `deskwaifu`; the CLI, IPC socket and config paths
are unchanged. Do not run the two backends against the same runtime socket.
The backend rejects non-Wayland platforms or compositors without layer-shell
instead of silently presenting an ordinary desktop window.

## Boundaries and behavior

- `DesktopWindow` is the application's window/output/input port.
  `NativeDesktopWindow` delegates to the existing Wayland/EGL implementation;
  `QtDesktopWindow` uses `QWindow`, `QOpenGLContext` and LayerShellQt. Qt owns its
  connection and surface: the application never dispatches Qt's Wayland socket
  or manages Qt's EGL handles itself.
- A Qt timer drives the shared frame callback through the Qt event loop. IPC and
  config polling retain their existing semantics; this phase does not replace
  their transports with Qt alternatives. Empty-input frames use a slower timer.
- The GL compatibility profile is necessary because the current Cubism/sprite
  renderer uses client-side vertex/index arrays. A core-only or OpenGL ES context
  is rejected. The integration test catches accidental core-context creation.
- The Qt backend renders into an explicitly supplied FBO, never assumes that the
  model's framebuffer is zero, and scales only final presentation to the backing
  buffer. Crop rectangles, input masks and mouse events remain in **logical
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
- `QScreen` provides logical output geometry and stable IDs are assigned for the
  lifetime of each screen. A screen replacement reuses the original GL context;
  input callbacks are suppressed while the replacement is being configured.
  Removing the current screen falls back to another screen; re-added screens
  receive fresh IDs. Model resources are released with the context current,
  before Qt destroys the context/window.
- Global cursor gaze still uses the existing KWin bridge. Focused-output and
  cross-output drag capabilities retain their compositor-specific limitations.
  Qt does not grant global-pointer access on Wayland. Sway/Hyprland behavior still
  needs real-compositor validation before making this backend the default.

## Validation

```sh
ctest --test-dir build-qt --output-on-failure
python3 tests/qt_headless_smoke.py build-qt /path/to/CubismSdkForNative-5-r.5
python3 tests/qt_headless_smoke.py build-qt /path/to/CubismSdkForNative-5-r.5 --scale 1.5
```

The smoke runner requires `kwin_wayland` and `dbus-run-session`; it creates a
private D-Bus session, two virtual outputs, runtime socket and config directory,
with software rendering. If `kscreen-doctor` is installed it also disables and
re-enables an output to exercise hotplug. It does not send input to the user's
desktop. Logs remain in `BUILD_DIR/qt-smoke-logs/SCALE/`.

The window integration executable verifies GL color/alpha, logical input masks,
empty-input restoration, synthetic Qt mouse/key dispatch, keyboard focus loss,
output switching and ordered teardown. The runner additionally checks actual
Wayland input-region requests and runs a real SDK model through motion,
expression, mouth/look, transform/crop saves, screen-edit, visibility, model
switch and quit IPC. This does **not** replace manual checks of physical pointer
click-through, compositor focus policy or visual quality on real desktops.

For the native model/IPC baseline, use the same runner with a native build and
`--native`. Both backends currently emit the SDK's existing
`GL_INVALID_VALUE in glDeleteProgram` teardown diagnostic (shader sets share
program IDs). The smoke test allows only that exact diagnostic **after quit**;
OpenGL errors during rendering still fail the test. No third-party SDK source
is modified as part of the backend migration.

The optional `DESKWAIFU_QT_LIVE_TEST=ON` CMake switch registers the window test
with CTest against the selected display. Use it only with an isolated compositor,
never against the user's active session.
