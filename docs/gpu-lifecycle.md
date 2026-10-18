# GPU ownership and teardown (#81)

The texture store owns GL texture objects. A model owns move-only
`TextureResidency::Lease` values keyed by asset path, and the renderer only
borrows numeric IDs while the leases are alive. A second model referencing the
same path acquires a second reference, not a second GPU upload. The last lease
releases the GL object immediately. Failed partial loads release their leases
when the partially built model is destroyed. Renderer recreation reuses the
model's existing leases. View offscreen targets and sprites are destroyed
before models, while the texture store outlives all borrowed leases.

The model session has a **128 MiB soft budget** for cached model texture
footprints (RGBA8 width × height × 4). It may temporarily exceed that during
loading, and a single active model larger than the budget remains pinned.
Otherwise it drops inactive cached models, and if necessary the prefetched
next model. Counts are deliberately conservative when paths are shared; the
texture store reports actual deduplicated residency and retains *no unleased
model texture*. Model count alone is not used as a memory budget.

Teardown binds the **original Qt OpenGL context current on the
calling thread** before destroying the View, model/session, offscreen targets,
texture store and Cubism framework; only then are Qt's FBO, context and window destroyed.
Before GPU initialization no binding is attempted. If a live GPU context
cannot be made current, teardown reports failure and intentionally leaves
dependent objects to process-level cleanup rather than invoking invalid GL
calls. The `quit` IPC command exercises successful normal exit under headless
Wayland CI (with an exit-code and teardown log assertion); fake GPU tests
verify the failed-bind and partial-initialization paths.
