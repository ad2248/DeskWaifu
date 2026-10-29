# Component dependencies

The authoritative ownership contract is [plugin architecture](plugin-architecture.md).
The control process and its page ownership are described in [GUI plugins](gui-plugins.md).

Production ownership (indentation means ownership, not calls or build links):

```text
platform/main → plugin discovery/lifecycle + SDK event loop
desktop plugin → private app/LAppDelegate (composition and lifecycle only)
  ├─ InteractionProcessor
  │    ├─ InteractionContext
  │    │    ├─ window/QtDesktopWindow, output/compositor helpers
  │    │    └─ ApplicationService, TransformEditor, config
  │    └─ local command adapter
  ├─ ModelProcessor
  │    └─ ModelRuntime (created, used and destroyed on model-render)
  │         └─ View, Manager, model/texture store, Cubism/GL resources
  └─ InputListener + SignalProcessor
ipc plugin → IpcProcessor → LAppIPC
  ⇄ CommandProcessor ⇄ public command.<name>/v1 providers
perception plugin → image / OCR / screenshot
speech plugin → SpeechEndpoint → speech processor / player / source / audio
reading_clip plugin → ReadingClipProcessor → public image / OCR / speech capabilities
```

Plugins never compile, link, include or call another owner's implementation, even
through Delegate or injected business callbacks. Strongly coupled model/window/input
helpers collaborate within desktop. Internal processor events use `ProcessorEvent`
values and opaque `IProcessorPlugin*` reply routes; cross-plugin requests use SDK
endpoints and public capabilities. LAppIPC calls the `ICommandService` implemented
by its owning IpcProcessor; that interface is not a separately owned service.
IpcProcessor and CommandProcessor exchange `command.request` / `ipc.command.result`.
Command routes only through `command.<name>/v1`, returning explicit provider errors
instead of falling back to built-in domains. Desktop's adapter checks model requests
through interaction before forwarding `model.operation` with the original sender.
Command owns protocol queues, not GUI, configuration, OCR or model state. Interaction
caches model metadata from `model.state`. Reading uses `image.read/v1`,
`ocr.recognize/v1` and `speech.speak/v1` through its plugin endpoint. Request IDs,
cancellation and timeouts are distinct from the plugin-local processor reply routes.

Build links are separate from this ownership/event graph. `deskwaifu` compiles
only platform startup/configuration and links the platform plugin host plus SDK/Qt.
It does not compile QtDesktopWindow or link business implementations.
`desktop_plugin` compiles the desktop entry and Qt window, linking its private
`deskwaifu_application` and helper libraries. Perception, IPC, speech and reading
implementations are linked into their own modules. The no-peer-call rule is also
enforced at build boundaries, including tests; see root and plugin-local CMakeLists.
The separate control process loads GUI entries; its tabs belong to
`src/plugins/<name>/gui/`, not MainWindow.

Processor event handlers run on the GUI thread; ModelProcessor is a GUI-side
proxy. A worker timer drives ModelRuntime on `model-render`, without per-frame
business events. GUI presentation uploads CPU RGBA frames. Image snapshots are
read on the GUI thread and encoded via QtConcurrent; OCR runs in a child process.

Desktop-private `processor/model/ModelExchange` is a bounded
latest-input/latest-immutable-frame slot, not a service port. Across the GUI/model
boundary, the local `ModelHitTest` callback weakly references a CPU snapshot mailbox
and cannot access processors, Cubism or GL. These rendering, hit-test and alpha-region
algorithms are not installed SDK contracts. Continuous pointer samples also use
bounded slots; discrete inputs are barriers.

The platform invalidates all plugin contexts before stopping any plugin. Each
plugin stops its own sources and tasks; desktop joins the model thread before
destroying the model proxy and shutting down the window. Image encoding and OCR
child processes retire before their resource references are released. Historical
test-only lifecycle/hover helper copies live in owning plugin test directories,
not the SDK or production implementation.

`processor-boundaries` includes negative fixtures for direct/relative cross-domain
includes, forward-declared concrete peers, Delegate access and known callback
bypasses; it is not a complete C++ call-graph or arbitrary-callback safety proof.
`processor-ownership` exercises real dynamically loaded IPC/OCR/reading modules with
private sockets, synthetic image data and public model/speech capability fixtures;
`desktop-ownership` preserves native input, edit-guard and session-release coverage.
`reading-plugin` separately loads actual reading/perception modules and a speech
capability fixture. `model-render-thread` supplies the real Cubism/GL half of the
contract. Host, endpoint and GUI tests cover the separate routing/lifecycle
boundaries; processor fixtures alone do not prove them.
