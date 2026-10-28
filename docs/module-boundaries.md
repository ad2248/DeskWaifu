# Module responsibilities (#77)

`src/plugins/desktop/private/app/LAppDelegate` is only desktop's composition/lifecycle root. InteractionProcessor
owns the window/output/configuration state; its backend is QtDesktopWindow.
See [plugin ownership](plugin-architecture.md) for the current event graph.
The desktop composition and model execution paths are shown below. These are
assembly/call relationships, not a complete build dependency or ownership graph.

```text
desktop/Delegate (wiring and lifecycle only)
  ├── InteractionProcessor → deskwaifu_input (pointer-to-model transform and tap policy)
  ├── InputListener + SignalProcessor
  └── ModelProcessor → model-render thread → ModelRuntime
        ├── View → LAppLive2DManager (active model, hit handling)
        ├── texture store
        └── LAppLive2DManager
              ├── deskwaifu_catalog (stable path IDs, deterministic ordering)
              ├── deskwaifu_loader → catalog descriptor, model/resource store
              ├── ModelSession → ModelCache (owner, prefetch and eviction)
              └── deskwaifu_renderer (one GL frame, projection, pre/post hooks)
                    → borrowed active models (normally session-owned)
```

ModelRuntime creates View, the manager and the texture store on the model-render
thread; model loading, updates, GL drawing and teardown also run on that thread.
The manager calls the renderer; ModelSession and ModelCache do not depend on it.

The catalog and input helpers remain internal to desktop. They use the shared
plugin SDK, but do not directly link GL or Wayland. The independent IPC plugin
has no desktop dependency; CommandProcessor routes only through public
`command.<name>/v1` capabilities, returning explicit errors for missing or ambiguous
providers. Both transport and its local adapter
include the small `ICommandService.hpp` contract; the transport does not include
the concrete handler header. The renderer and loader
depend on Cubism. The manager keeps cached model pointers private, with
`ModelSession` owning them; View borrows only active pointers for the duration
of a frame. With `USE_RENDER_TARGET` or `USE_MODEL_RENDER_TARGET`, the manager
also owns an extra sample model outside the session and includes it in drawing.
A failed selection keeps the previous model active, while failed prefetch is
non-fatal. Selection synchronously prefetches the next index; with more than four
catalog entries, ordinary eviction keeps only the current and next models.
The default 128 MiB texture soft budget can evict other cached models, including
the prefetched one, but always retains the active model. Shared textures count
separately per model for this budget; it is not a hard GPU memory limit.
Models with duplicate visible names have distinct
canonical-directory-plus-filename IDs (the JSON file may symlink to shared
assets without merging model sessions); the existing name-based IPC lookup still
selects the last sorted match, because each matching name overwrites the selected
index. Startup `defaultModel` lookup instead selects the first sorted match.

`BUILD_TESTING=ON` builds and registers the enabled plugins' catalog, cache,
session, input, dependency-direction and IPC transport tests even when
`DESKWAIFU_BUILD_CONTROL=OFF`;
CTest executes them. The Qt model window and its ordinary tests remain enabled,
but the full `qt-desktop-window` and `gpu-transfer` integration tests additionally
require `DESKWAIFU_QT_LIVE_TEST=ON`. Disabling the control application also excludes
launcher/respawner, Qt client/panel, model-hotkeys, model-transform and model-keyboard
tests. Speech contract and component tests remain independent of CONTROL but require
`DESKWAIFU_BUILD_SPEECH_PLUGIN=ON`.
`DESKWAIFU_TEST_GATE=ON` requires BUILD_TESTING, CONTROL and all five backend
plugin build options enabled. Configuration
validation, transform/visibility mutation and persistent/stream external controls
belong to the GUI interaction owner. Model state/actions belong to ModelProcessor;
platform-specific output selection is internal to the GUI owner.

Speech credentials and reading orchestration belong to backend plugins, not the
GUI interaction owner. The control process loads optional GUI plugin entries;
those pages use IPC rather than backend objects. GUI-only system/interaction/model
modules provide pages; model/window/input backend business is owned by desktop,
not those GUI modules or the platform.
See [GUI plugin ownership](gui-plugins.md).
