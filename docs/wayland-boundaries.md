# Wayland integration (#80)

- `QtDesktopWindow` uses Qt and LayerShellQt for the window, context and surface
  transitions. It translates pointer, keyboard and close events into
  `IDesktopWindowEvents`, a borrowed application-owned sink. No window callback
  gets an application singleton. It assigns stable IDs to QScreen objects;
  removing an output does not
  shift any remaining identity. If the current output disappears, the backend
  rebinds to a remaining output or closes the app when none remain.
- `CompositorAdapter` alone queries Hyprland or Sway. Its transport limits
  command/socket reads to 16 KiB and 250 ms and reports `Available`,
  `Unsupported`, `Unavailable` or `Failed` distinctly. The generic backend
  still receives surface-local pointer/keyboard events. For Hyprland the
  optional global cursor helps detect drag-to-output; Sway supports focused
  output switching via SIGUSR2. Missing capabilities do not fake coordinates.
- `OutputController` selects from copied `OutputSnapshot` values, using
   stable `OutputId` instead of vector position; only the window handles QScreen
   objects. Selection failure leaves transform unchanged. Successful
   cross-output dragging scales with logical output height and clamps zoom to
  the application's valid interval before committing a transform.
- `OutputSettings` selects an output by connector name for startup and control-panel
  requests. Explicit switches persist the name together with centered coordinates
  and a cleared crop, preserving zoom. An unavailable startup name falls back to
  the first enumerated output and saves that replacement. Transform editing blocks
  explicit selection; a failed save attempts to restore the previous output.
  Startup fallback persistence is best-effort: a write failure logs a warning but
  keeps the available output and readable configuration's placement usable.

Qt/LayerShellQt handle protocol configure acknowledgements and EGL integration.
The application waits for the Qt window to be exposed before binding its
QOpenGLContext. Output replacement retains that context and rolls back to the
previous screen on failure where possible. Shutdown rebinds the same context
before deleting model resources, then releases Qt's FBO, context and window.
The headless smoke test exercises a live model on two Sway headless outputs,
both switches, IPC liveness and graceful GPU shutdown.
