# Component dependencies

For legacy processor behavior see [processor/README](../src/processor/README.md);
the current plugin boundaries and migration scope are described in
[plugin architecture](plugin-architecture.md) and [GUI plugins](gui-plugins.md).

Production ownership (indentation means ownership, not calls or build links):

```text
app/LAppDelegate (constructed and run by main; composition and lifecycle only)
  ├─ InteractionProcessor
  │    ├─ InteractionContext
  │    │    ├─ window/QtDesktopWindow, output/compositor helpers
  │    │    └─ ApplicationService, TransformEditor, config
  │    └─ local command adapter
  ├─ ModelProcessor
  │    └─ ModelRuntime (created, used and destroyed on model-render)
  │         └─ View, Manager, model/texture store, Cubism/GL resources
  ├─ IpcProcessor
  │    └─ LAppIPC
  ├─ CommandProcessor, InputListener, SignalProcessor
  ├─ PluginManager
  │    └─ backend plugin instances and contexts
  │         ├─ reading_clip → ReadingClipProcessor
  │         └─ speech (optional) → SpeechEndpoint → SpeechProcessor/player/source/audio
  └─ ProcessorRuntime
       └─ event loop; image / OCR / screenshot processors
```

Concrete processors never include or call a peer implementation, even through
Delegate or injected business callbacks. They exchange `ProcessorEvent` values
and opaque `IProcessor*` reply routes. Internal helper classes may call each
other within their owner. LAppIPC calls the `ICommandService` implemented by its
owning IpcProcessor; the interface is not a separately owned service.
IpcProcessor and CommandProcessor exchange `command.request` / `ipc.command.result`.
Command first requests the dynamic `command.<name>/v1` capability. Only when no
provider exists does it fall back to `interaction.command`, `interaction.model`
or `ocr.command`; interaction checks legacy model requests before
forwarding `model.operation` with the original sender. Command owns protocol
queues, not GUI, configuration, OCR or model state. Interaction caches model
metadata from `model.state`. Reading uses `image.read/v1`, `ocr.recognize/v1`
and `speech.speak/v1` through its plugin endpoint. Plugin request IDs, cancellation
and timeouts are distinct from legacy `IProcessor*` reply routes.

Build links are separate from this ownership/event graph. `deskwaifu` compiles
main and QtDesktopWindow and links `deskwaifu_application`, which compiles the
composition root and model implementation and links `deskwaifu_processors`,
`deskwaifu_plugin_host` and helper libraries. `deskwaifu_processors` groups multiple domains in one static
library and publicly links event, OCR, screenshot, IPC, input, output,
compositor, offscreen, window-state and cursor libraries. The no-peer-call rule
does not imply separate build targets (see the root, `src` and `src/processor`
CMakeLists). Speech and reading implementations are linked into their loadable
modules rather than the host's processor runtime. The separate control process
loads GUI entries; its tabs belong to `src/plugins/<name>/gui/`, not MainWindow.

Processor event handlers run on the GUI thread; ModelProcessor is a GUI-side
proxy. A worker timer drives ModelRuntime on `model-render`, without per-frame
business events. GUI presentation uploads CPU RGBA frames. Image snapshots are
read on the GUI thread and encoded via QtConcurrent; OCR runs in a child process.

`contracts/ModelExchange` is a bounded latest-input/latest-immutable-frame slot,
not a service port. Across the GUI/model boundary, `ModelHitTest` is the permitted
query callback: it weakly
references a CPU snapshot mailbox and cannot access processors, SDK or GL.
Continuous pointer samples also use bounded slots; discrete inputs are barriers.

The composition root stops signal/IPC/input/interaction sources, cancels command
requests, stops/destroys backend plugins, and joins the model thread before
destroying the model proxy and shutting down the window. InputListener unregisters
during Stop. ProcessorRuntime
is destroyed after the window; it stops its services before member destruction
unregisters them. Image encoding and OCR child processes retire before their
resource references are released. Historical lifecycle/hover test copies have
been retired; they no longer live in `tests/support`.

`processor-boundaries` includes negative fixtures for direct/relative cross-domain
includes, forward-declared concrete peers, Delegate access and known callback
bypasses; it is not a complete C++ call-graph or arbitrary-callback safety proof.
`processor-ownership` exercises IPC/Command/GUI/OCR with injected reading wiring and
private sockets, synthetic image data and fake platform/model/speech boundaries;
`reading-plugin` separately loads the actual reading module, using real image/OCR
and a speech capability fixture. `model-render-thread` supplies the real Cubism/GL
half of the contract. Plugin host, endpoint and GUI tests cover the separate
plugin routing/lifecycle boundaries; processor fixtures alone do not prove them.
