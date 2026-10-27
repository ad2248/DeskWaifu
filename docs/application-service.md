# Application operations (#78)

`ApplicationService` is an internal helper of InteractionProcessor and the single
writer for its application transform, hidden flag and external control sources,
not for all window/backend state. The owned InteractionContext implements its
local runtime port and holds the window,
configuration and editor. Model selection/actions belong to ModelProcessor and
are requested only through events; model metadata is cached from state events.
Delegate only assembles lifetimes and never forwards business calls.
Speech credentials and clipboard-reading orchestration now belong to backend
plugins, not ApplicationService or InteractionContext. CommandProcessor first
tries plugin command capabilities before legacy domain routing; control-panel
GUI entries use IPC, not direct references to this service. See
[plugin architecture](plugin-architecture.md) and [GUI plugins](gui-plugins.md).

`SetTransform` rejects non-finite coordinates/scale and zoom outside
`[0.1, 10]` without changing any part of the previous value. Model selection
commits only after loading succeeds. Motion requests report unavailable choices
or failed starts. Expression requests reject names absent from the expression
list, but a listed name does not guarantee a loaded expression or a successful
start. The snapshot is a value, safe to use for one frame or response without
retaining a reference to runtime state.

On hide, the runtime first asks the Qt backend for an empty input region, then
requests release of keyboard exclusivity and queues input reset events. The
service commits its hidden flag after the runtime call succeeds; this does not
wait for compositor acknowledgement or queued key cleanup, and the hide path
does not check the keyboard-release result. Presentation rebuilds the content
input region from a frame matching the current dimensions, hidden flag and model
index; without one it uses no content regions. Screen editing uses a separate
input-region policy and can retain input while hidden. A POSIX signal handler
writes to a nonblocking self-pipe; SignalProcessor emits a normal event consumed by the GUI
owner. Neither it nor CommandProcessor receives a service reference or callback.

Catalog/status queries serialize cached state; hotkey queries/actions use model
request/result events. External controls are arbitrated per channel: streaming
sources outrank persistent sources, and the latest update wins within a rank.
Disconnect releases that connection's controls; a model index or instance-token
change clears all controls. The shared frame slot contains value input,
immutable CPU frames and an error status, not window or GPU objects.
See [processor ownership](../src/processor/README.md).
