# GPU ownership and teardown (#81)

The texture store owns model GL texture objects. A model owns move-only
`TextureResidency::Lease` values keyed by asset path, and the renderer only
borrows numeric IDs while the leases are alive. A second model referencing the
same exact path string acquires a second reference, not a second GPU upload;
path aliases are not normalized. The last lease calls `glDeleteTextures`.
Detected partial-load failures release their leases when the partially built
model is destroyed. Read/decode failures and zero texture IDs are detected,
but upload and mipmap GL errors are not checked. Renderer recreation reuses the
model's existing leases. View offscreen targets and sprites are destroyed
before models, while the texture store outlives all borrowed leases.

The model session has a **128 MiB soft budget** for cached model texture
footprints (RGBA8 base level: width × height × 4). It may temporarily exceed that during
loading, and a single active model larger than the budget remains pinned.
Otherwise it drops inactive cached models, and if necessary the prefetched
next model. Counts are deliberately conservative when paths are shared; the
texture store reports the deduplicated base-level estimate and retains *no
unleased model texture*. These estimates are not actual VRAM usage: they exclude
generated mipmaps, render targets, SDK-internal resources and driver overhead.
Separately, catalogs larger than four models discard all but the selected and
next model before prefetching, regardless of the byte budget.

Model teardown runs on the **model-render worker thread**, restoring its own
offscreen context: Qt by default, EGL for an explicitly selected device, not
the GUI window's presentation context. Successful runtime teardown destroys
the View, model/session, texture store and Cubism framework in that order;
the render target then releases its FBO and context. GUI window resources are
shut down afterward. Prepare failure creates no worker; once a worker exists,
even initialization failure attempts context binding during Stop.

If runtime context restoration fails, Stop reports failure, abandons the View,
manager/session and texture store to process cleanup, and skips Cubism cleanup.
The render target still attempts its own independent shutdown. A Qt target
that cannot bind abandons its FBO but destroys its context; an EGL target whose
shutdown fails is abandoned as a whole. GUI window shutdown still proceeds,
deleting its FBO if binding succeeds or abandoning it otherwise, then destroying
its context and window.

Headless Wayland CI defines a `quit` IPC smoke test with exit-code and teardown
log assertions; it permits a specific known SDK `glDeleteProgram` diagnostic.
`texture-residency` and `model-session` define lease/accounting and soft-budget
checks. `model-render-thread` and `model-shutdown-context` use real Cubism and
software EGL with injected failures, observing actual texture operations and
thread/context identity for normal shutdown, partial initialization and failed
runtime restoration. They do not prove every shader/FBO operation, a complete
destructor order, or physical context-loss recovery. These are test definitions,
not a claim that the tests were run for this document update.
