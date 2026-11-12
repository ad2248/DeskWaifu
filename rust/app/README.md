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
| `renderer_error` | why rendering failed, or `null` |

`debug_frame` forces one frame plus a fresh readback and returns the same
counts; passing `{"args":{"id":"ParamAngleX"}}` also reports the **rendered**
model's current value for that parameter. It is the test seam for the live
compositor test.

## Known gaps

- **Input region**: the surface is fully click-through (an empty region). The C++
  window derives a shaped region from the rendered alpha via
  `deskwaifu_model_domain::extract_input_regions`; that analysis, its screen-edit
  mode and the pointer interaction above it are not implemented yet.
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
transparent pixels, that `set_parameter` changes the drawn frame, and that a
model switch keeps rendering. Its non-live test proves `--overlay` without
`WAYLAND_DISPLAY` fails with a clear error instead of hanging.
