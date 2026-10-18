# Module responsibilities (#77)

`LAppDelegate` remains the composition root; its window/output port is implemented
only by `QtDesktopWindow`. Core responsibilities are split
as follows; the arrows below are build dependencies, not ownership transfers.

```text
App/Delegate (wiring, event loop)
  ├── deskwaifu_ipc (Unix socket, connection lifecycle, line framing)
  │     → ICommandService (command port; concrete AppCommandHandler)
  ├── deskwaifu_input (pointer-to-model transform and tap policy)
  ├── View → LAppLive2DManager (ModelSession façade: active model, hit handling)
  │     ├── deskwaifu_catalog (stable path IDs, deterministic ordering)
  │     ├── deskwaifu_loader → catalog descriptor, model/resource store
  │     └── ModelSession → ModelCache (owner, prefetch and eviction)
  │         └── deskwaifu_renderer (one GL frame, projection, pre/post hooks)
  │               → borrowed models from session
```

The catalog and input library do not link SDK, GL, Qt or Wayland. The IPC
transport links none of these either; the application command handler is the
only module interpreting JSON as model operations. The renderer and loader
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
validation, state mutation invariants and visibility are owned by the future
ApplicationService (#78); persistent/stream external controls belong to #79;
platform-specific output selection to #80 and GPU lease accounting to #81.
