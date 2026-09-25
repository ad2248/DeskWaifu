# Core component dependencies (#76)

The application Delegate composes and owns the texture store, model manager,
and View (in that construction order). Its main loop obtains the IPC input
values for a frame and passes a **value snapshot** to View; it also supplies
the current transform. Model and Pal have no reason to look up Delegate.

```text
main → Delegate (composition/lifetime)
         ├─ texture store
         ├─ Manager → Model → borrowed texture store, ModelFrameInput
         └─ View → Manager (render/touch) and borrowed texture store

IPC → Delegate → Manager          (legacy command access; #77/#78 will extract it)
Wayland callbacks → Delegate     (legacy event access; #80 will extract it)
Pal → explicit resource base path (configured before Cubism startup)
```

Manager owns cached models; View and models borrow resources and must not
outlive Manager and the texture store respectively. Drawing hooks are passed
to Manager per frame, so Manager does not include or look up View. Frame
control values are passed through Delegate → View → Manager → Model; the
Model never includes IPC. The manager receives model directories, default
model, expression timeout, viewport and texture store at construction;
viewport changes use `SetRenderTargetSize`. `ReleaseRuntimeInOrder` encodes
the logical reverse destruction order and supports partial initialization.

The current compositor callbacks and GPU residency rules are **not** covered
by this dependency extraction; their contracts belong to #80 and #81.
