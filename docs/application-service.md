# Application operations (#78)

`ApplicationService` is an internal helper of InteractionProcessor and the single
writer for GUI transform, visibility and external control sources. The owned
InteractionContext implements its local runtime port and holds the window,
configuration and editor. Model selection/actions belong to ModelProcessor and
are requested only through events; model metadata is cached from state events.
Delegate only assembles lifetimes and never forwards business calls.

`SetTransform` rejects non-finite coordinates/scale and zoom outside
`[0.1, 10]` without changing any part of the previous value. Model selection
commits only after loading succeeds. Motion and expression reject unavailable
choices rather than pretending success. The snapshot is a value, safe to use
for one frame or response without retaining a reference to runtime state.

On hide, the backend first submits an empty Wayland input region, then releases
keyboard exclusivity and clears pressed keys. The service reports success only
after the backend accepts that transition; the next visible frame rebuilds the
input region from the rendered content. A POSIX signal handler writes to a
nonblocking self-pipe; SignalProcessor emits a normal event consumed by the GUI
owner. Neither it nor CommandProcessor receives a service reference or callback.

Catalog/status queries serialize cached state; hotkey queries/actions use model
request/result events. The shared frame slot contains only value input and
immutable CPU frames. See [processor ownership](../src/processor/README.md).
