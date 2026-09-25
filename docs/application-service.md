# Application operations (#78)

`ApplicationService` is the synchronous single writer for model transform,
visibility, model selection, expression, motion, skin and hotkey operations.
`LAppDelegate` implements the runtime port and owns the service. Inputs from
startup configuration, pointer/scroll, output switching, signals and JSON IPC
all call the same operations. The transport adapter translates the result
into an IPC response; it cannot write transform or visibility fields.

`SetTransform` rejects non-finite coordinates/scale and zoom outside
`[0.1, 10]` without changing any part of the previous value. Model selection
commits only after loading succeeds. Motion and expression reject unavailable
choices rather than pretending success. The snapshot is a value, safe to use
for one frame or response without retaining a reference to runtime state.

On hide, the backend first submits an empty Wayland input region, then releases
keyboard exclusivity and clears pressed keys. The service reports success only
after the backend accepts that transition; the next visible frame rebuilds the
input region from the rendered content. A signal handler only sets a
`sig_atomic_t` request flag; the event loop performs the service operation.

Model catalog queries and hotkey metadata serialization remain read-only in
the IPC adapter. External look/mouth source arbitration remains #79; the
Wayland output lifecycle and GPU ownership remain #80/#81 respectively.
