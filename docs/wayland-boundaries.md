# Wayland integration (#80)

- `QtDesktopWindow` uses Qt and LayerShellQt for the window, presentation context and surface
  transitions. It translates pointer, keyboard and close events into
  `IDesktopWindowEvents`, a borrowed application-owned sink. No window callback
  gets an application singleton. IDs are stable for each QScreen lifetime;
  removing an output does not shift remaining identities, but reconnecting it
  assigns a new ID. If the current output disappears, the backend rebinds to the
  first remaining output or requests app closure if none remain or rebinding fails.
- `CompositorAdapter` alone queries Hyprland or Sway. Response reads have a
  250 ms deadline and require EOF before reaching 16 KiB; reaching that limit
  fails. Commands share a 250 ms budget for reading and waiting for child exit,
  with a separate 250 ms cleanup wait before deferred reaping. Hyprland socket
  connection readiness and response reading each have a 250 ms budget, not a
  single end-to-end deadline. Capabilities report `Available`, `Unsupported`,
  `Unavailable` or `Failed`, but command read failures (including oversized
  responses) are folded into `Unavailable`; socket reads preserve `Failed`.
  The generic backend still receives surface-local pointer/keyboard events.
  Hyprland's optional global cursor detects cross-output model dragging on
  release during local screen editing. Hyprland and Sway support focused-output
  switching via SIGUSR2; this does not persist the output selection.
  Missing capabilities do not fake coordinates.
- `InputListener` separately uses the KWin D-Bus/script cursor bridge for global
  gaze, sampling on a 16 ms timer independently of presentation. Valid samples
  are mapped to the current output's local coordinates; unavailable or stale
  samples leave surface events usable, and the bridge retries loading.
  This does not provide KWin focused-output switching or cross-output dragging;
  `CompositorAdapter` treats KWin as generic.
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
presentation QOpenGLContext. Output replacement retains that context and rolls
back to the previous screen on failure where possible. Model resources belong
to a separate render-thread offscreen context (Qt by default, explicit EGL device
when selected); RGBA frames are read back and uploaded for window presentation.
Shutdown first rebinds the model context and releases model resources and the
render target, then releases the presentation FBO, context and window. If the
required context cannot be rebound, GPU cleanup may be left to process exit.

The Sway headless smoke test exercises a live model on two outputs, switches in
both directions, IPC liveness and graceful GPU shutdown. Separate private KWin
tests cover two-output window switching, active-output removal/re-addition when
`kscreen-doctor` is available, keyboard focus, actual click-through and GPU frame
transfer. CI runs the KWin smoke at scales 1 and 1.5, with the latter also testing
the installed application. Unit tests cover output selection/transform policy,
settings persistence and rollback, compositor transport failures and KWin cursor
bridge recovery.
