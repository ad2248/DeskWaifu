# Component dependencies

The authoritative ownership contract is [plugin architecture](plugin-architecture.md).

```text
platform/main → plugin discovery/lifecycle + SDK event loop
desktop plugin → private app/LAppDelegate
  ├─ InteractionProcessor → owned InteractionContext
  │    ├─ window/QtDesktopWindow, output/compositor helpers
  │    └─ ApplicationService, TransformEditor, config/*, local command adapter
  ├─ ModelProcessor → model-render thread → ModelRuntime
  │    └─ View, Manager, model/texture store, Cubism/GL resources
  └─ InputListener + SignalProcessor
ipc plugin → IpcProcessor → LAppIPC → local ICommandService
  ⇄ CommandProcessor ⇄ public command.<name>/v1 providers
perception plugin → image / OCR / screenshot
speech plugin → speech player / source / audio
reading_clip plugin → public image / OCR / speech capabilities
```

Plugins never compile, link, include or call another owner's implementation, even through
Delegate or injected business callbacks. They exchange SDK events and capability requests.
Strongly coupled model/window/input helpers collaborate within desktop. Command routes protocol requests but owns no GUI,
configuration, OCR or model state. Model metadata is cached from `model.state`.

`contracts/ModelExchange` is a bounded latest-input/latest-immutable-frame slot,
not a service port. `ModelHitTest` is the one permitted query callback: it weakly
references a CPU snapshot mailbox and cannot access processors, SDK or GL.
Continuous pointer samples also use bounded slots; discrete inputs are barriers.

Each plugin stops its sources and tasks. Desktop joins the model thread before
unregistering consumers and destroying the window. Image
encoding and OCR child processes retire before their resource references are
released. Historical test-only lifecycle/hover helper templates live in
owning plugin test directories, not the SDK or production implementation.

`processor-boundaries` includes negative fixtures for direct/relative cross-domain
includes, forward-declared concrete peers, Delegate access and callback bypasses.
`processor-ownership` exercises real dynamically loaded IPC/OCR/reading modules with
private sockets, synthetic image data and public model/speech capability fixtures;
`desktop-ownership` preserves native input, edit-guard and session-release coverage;
`model-render-thread` supplies the real Cubism/GL half of the contract.
