# Module responsibilities (#77)

`app/LAppDelegate` is only the composition/lifecycle root. InteractionProcessor
owns the window/output/configuration state; its backend is QtDesktopWindow.
See [processor ownership](../src/processor/README.md) for the current event graph.
The lower-level model libraries remain split
as follows; the arrows below are build dependencies, not ownership transfers.

```text
App/Delegate (wiring and lifecycle only)
  ├── IpcProcessor → deskwaifu_ipc (socket, connection lifecycle, line framing)
  │     → local ICommandService → events → CommandProcessor → domain events
  ├── deskwaifu_input (pointer-to-model transform and tap policy)
  ├── View → LAppLive2DManager (ModelSession façade: active model, hit handling)
  │     ├── deskwaifu_catalog (stable path IDs, deterministic ordering)
  │     ├── deskwaifu_loader → catalog descriptor, model/resource store
  │     └── ModelSession → ModelCache (owner, prefetch and eviction)
  │         └── deskwaifu_renderer (one GL frame, projection, pre/post hooks)
  │               → borrowed models from session
```

The catalog and input library do not link SDK, GL, Qt or Wayland. The IPC
transport links none of these either; CommandProcessor routes protocol requests
to their actual domain owners. Both transport and its local adapter
include the small `ICommandService.hpp` contract; the transport does not include
the concrete handler header. The renderer and loader
depend on Cubism. The manager keeps cached model pointers private, with
`ModelSession` owning them; View borrows only active pointers for the duration
of a frame. A failed selection keeps the previous model active, while failed
prefetch is non-fatal. Models with duplicate visible names have distinct
canonical-directory-plus-filename IDs (the JSON file may symlink to shared
assets without merging model sessions); the existing name-based IPC lookup still
selects the first sorted match for compatibility.

`BUILD_TESTING=ON` runs catalog, cache, session, input, dependency direction
and IPC transport tests even when `DESKWAIFU_BUILD_CONTROL=OFF`. The Qt model
window and its tests remain enabled in that configuration. The Qt client
and panel tests are gated on the control application only. Configuration
validation, transform/visibility mutation and persistent/stream external controls
belong to the GUI interaction owner. Model state/actions belong to ModelProcessor;
platform-specific output selection is internal to the GUI owner.
