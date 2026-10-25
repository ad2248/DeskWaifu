# Component dependencies

The authoritative ownership/event contract is [processor/README](../src/processor/README.md).

```text
main → app/LAppDelegate (construction and lifecycle only)
  ├─ InteractionProcessor → owned InteractionContext
  │    ├─ window/QtDesktopWindow, output/compositor helpers
  │    └─ ApplicationService, TransformEditor, config/*, local command adapter
  ├─ ModelProcessor → model-render thread → ModelRuntime
  │    └─ View, Manager, model/texture store, Cubism/GL resources
  ├─ IpcProcessor → owned LAppIPC → local ICommandService
  │    ⇄ command.request / ipc.command.result ⇄ CommandProcessor
  └─ ProcessorRuntime → image / OCR / screenshot / speech / reading processors
```

Concrete processors never include or call a peer implementation, even through
Delegate or injected business callbacks. They exchange `ProcessorEvent` values
and opaque `IProcessor*` reply routes. Internal helper classes may call each
other within their owner. Command routes protocol requests but owns no GUI,
configuration, OCR or model state. Model metadata is cached from `model.state`.

`contracts/ModelExchange` is a bounded latest-input/latest-immutable-frame slot,
not a service port. `ModelHitTest` is the one permitted query callback: it weakly
references a CPU snapshot mailbox and cannot access processors, SDK or GL.
Continuous pointer samples also use bounded slots; discrete inputs are barriers.

The composition root stops input/IPC sources, cancels pending work and joins the
model thread before unregistering consumers and destroying the window. Image
encoding and OCR child processes retire before their resource references are
released. Historical test-only lifecycle/hover helper templates live in
`tests/support`, not production `src`.

`processor-boundaries` includes negative fixtures for direct/relative cross-domain
includes, forward-declared concrete peers, Delegate access and callback bypasses.
`processor-ownership` exercises the real IPC/Command/GUI/OCR/reading wiring with
private sockets, synthetic image data and fake platform/model/speech boundaries;
`model-render-thread` supplies the real Cubism/GL half of the contract.
