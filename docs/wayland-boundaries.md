# Wayland integration (#80)

- `WaylandBackend` owns protocol objects and EGL surface transitions. It
  translates pointer, keyboard and close callbacks into `IWaylandEvents`, a
  borrowed application-owned sink. No protocol callback gets an application
  singleton. It snapshots wl_registry output IDs; removing an output does not
  shift any remaining identity. If the current output disappears, the backend
  rebinds to a remaining output or closes the app when none remain.
- `CompositorAdapter` alone queries Hyprland or Sway. Its transport limits
  command/socket reads to 16 KiB and 250 ms and reports `Available`,
  `Unsupported`, `Unavailable` or `Failed` distinctly. The generic backend
  still receives surface-local pointer/keyboard events. For Hyprland the
  optional global cursor helps detect drag-to-output; Sway supports focused
  output switching via SIGUSR2. Missing capabilities do not fake coordinates.
- `OutputController` selects from copied `OutputSnapshot` values, using
  stable `OutputId` instead of vector position; only the backend handles raw
  protocol objects. Selection failure leaves transform unchanged. Successful
  cross-output dragging scales with physical output height and clamps zoom to
  the application's valid interval before committing a transform.
- `OutputSettings` selects an output by connector name for startup and control-panel
  requests. Explicit switches persist the name together with centered coordinates
  and a cleared crop, preserving zoom. An unavailable startup name falls back to
  the first enumerated output and saves that replacement. Transform editing blocks
  explicit selection; a failed save attempts to restore the previous output.
  Startup fallback persistence is best-effort: a write failure logs a warning but
  keeps the available output and readable configuration's placement usable.

The initial layer surface and each replacement must acknowledge the first
`configure` before creating a corresponding EGL window. A replacement is
staged while the old surface/context remain live; a failed creation restores
the old surface. On success the original EGL context is rebound to the new
surface before the old EGL surface is destroyed. The `#81` shutdown path then
rebinds that same context before deleting GPU resources and EGL objects.
The headless smoke test exercises a live model on two Sway headless outputs,
both switches, IPC liveness and graceful GPU shutdown.
