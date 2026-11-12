# deskwaifu-rust

The Rust model runtime behind `deskwaifu-rust`. It owns the model catalog, the
Cubism Core model, the IPC socket, model metadata, hotkeys and external control
sources.

## Two modes

By default the runtime is **headless**: it loads the current model into Cubism
Core, answers IPC requests, and runs the same 16 ms update tick as the desktop
runtime. Nothing is drawn and no compositor is needed, which is what the CI and
most tests exercise.

### `--overlay` (opt-in rendering)

```sh
CUBISM_SDK_ROOT=/path/to/CubismSdkForNative-5-r.5 \
  deskwaifu-rust --overlay --model /path/to/Haru.model3.json --socket /tmp/model.sock
```

`--render` is accepted as an alias. With the flag the runtime additionally:

- connects to `WAYLAND_DISPLAY` and creates a `zwlr_layer_shell_v1` overlay
  (640x480, overlay layer, top-left anchor, no keyboard focus)
- starts the Cubism Framework with the SDK's external
  `Framework/src/Rendering/OpenGL/Shaders/Standard` directory and uploads the
  model's textures to GL
- draws one canvas-fitting frame per 16 ms tick, honouring `zoom`, `x`, `y` and
  `hidden` from the IPC transform commands
- derives the Wayland **input region** from every drawn frame's alpha, so the
  overlay is click-through exactly where the model is transparent (see below)
- rebuilds the rendered model, textures and renderer on `set_model`,
  `next_model` and `prev_model`
- plays the model's **first declared motion in a group named `Idle`**, if the
  model declares one, and blinks using the `Groups` entry named `EyeBlink`.
  No motion is invented when none is declared, and there is no random selection.

Failing to reach the compositor, start the Framework, read a texture or create
the renderer is a startup error: the process exits non-zero with the reason on
stderr instead of opening a blank window.

`--overlay` requires `WAYLAND_DISPLAY` and `CUBISM_SDK_ROOT`. Building the
runtime at all also requires `GLEW_ROOT` (CMake passes it; a plain
`cargo build`/`cargo test` in this directory needs it exported).

### Click-through

Only the drawn model receives pointer input. Every frame, after drawing and
before presenting, the runtime reads the drawn buffer back and derives the
surface-local input region from its alpha, then installs it with
`wl_surface.set_input_region`:

- `deskwaifu_model_domain::extract_input_regions` samples 10x10 blocks at their
  centre and treats alpha **above 10** as opaque, producing clipped
  top-left-origin runs — the same block size, threshold and clipping as the C++
  window's `ExtractModelInputRegion`.
- A fully transparent frame (for example while `toggle_hidden` hides the model)
  yields an empty region: the whole surface is click-through. A fully opaque
  frame yields regions covering the whole surface.
- Nothing is re-clipped or merged afterwards: `Overlay::set_input_region` only
  unions the rectangles into a `wl_region`.

The region is recomputed on **every** frame, which is what the C++ does: its
render thread reads the model framebuffer every 16 ms and its interaction thread
calls `SetInputRegion` on every presented frame, with no change detection. Since
the region follows the animated model, skipping frames would only save work on
frames that did not move. The C++ derives the region from the model framebuffer
*before* display opacity is applied, so hover dimming never changes
click-through; this runtime has no hover opacity, so the presented buffer is the
same buffer.

If `set_input_region` fails, the frame fails as a renderer error rather than
reporting a region that never reached the compositor.

### Status and diagnostics

`get_status` keeps its headless reply exactly as before. In overlay mode it adds:

| field | meaning |
| --- | --- |
| `renderer_available` | `true` only after a renderer was created **and** a frame was drawn |
| `frames_drawn` | frames presented so far |
| `visible_pixels` | pixels with alpha above the input-region threshold |
| `transparent_pixels` | fully transparent pixels |
| `pixel_checksum` | checksum of the last readback, for change detection |
| `width`, `height` | surface size |
| `input_regions` | rectangles in the applied input region (`0` = fully click-through) |
| `input_region_pixels` | total area those rectangles cover |
| `input_region_applied` | whether `set_input_region` accepted the region for that frame |
| `renderer_error` | why rendering failed, or `null` |

`debug_frame` forces one frame plus a fresh readback and returns the same
counts, plus the exact `input_region` rectangle list. Passing
`{"args":{"id":"ParamAngleX"}}` also reports the **rendered** model's current
value for that parameter. It is the test seam for the live compositor test.

## Known gaps

- **Pointer interaction and transform editing**: the input region decides which
  pixels are clickable, but no pointer events are consumed yet: there is no
  hit testing, dragging or tap motion. The C++ also widens the region to the
  whole surface while a transform edit is in progress
  (`ModelFrameInputRegions(true, ..)`); this runtime has no transform editor, so
  that mode does not exist rather than being faked.
- **Expressions, physics, pose, breathing, look-at**: `set_expression` and
  `do_motion` fail explicitly instead of claiming success. Only the `Idle` motion
  and eye blink are driven.
- **Window configuration**: size and anchors are fixed constants, not settings.
- **Textures are not shared** between model instances, so a model switch
  re-uploads every texture (the C++ `TextureResidency` lease does not exist here).

## Tests

```sh
export CUBISM_SDK_ROOT=/path/to/CubismSdkForNative-5-r.5
export GLEW_ROOT=/path/to/glew
export CUBISM_TEST_MODEL=$CUBISM_SDK_ROOT/Samples/Resources/Haru/Haru.model3.json
export LIBGL_ALWAYS_SOFTWARE=1
cargo test --locked
```

`tests/overlay.rs` skips the live compositor proof unless
`DESKWAIFU_RUNTIME_LIVE_TEST=1` (the same convention as `deskwaifu-overlay`); it
then spawns a private `kwin_wayland --virtual`, asserts real visible and
transparent pixels, that `set_parameter` changes the drawn frame, that a model
switch keeps rendering, and that the input region is nonempty, inside the
surface and non-overlapping for a drawn model, empty for a hidden one, and
restored when it is shown again. Its non-live test proves `--overlay` without
`WAYLAND_DISPLAY` fails with a clear error instead of hanging.

The alpha → rectangle mapping itself has pure unit tests in `src/render.rs`
(no compositor needed): fully transparent, fully opaque, a single opaque block,
the threshold alpha values 10 and 11, bottom-up row order, and run splitting
without overlap.
