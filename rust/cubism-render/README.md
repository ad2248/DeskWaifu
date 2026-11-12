# cubism-render

Narrow Rust facade over the external Live2D Cubism Framework **OpenGL renderer**.
It proves that a moc3 model can be loaded, parameterised and drawn into a
caller-owned GL framebuffer, and that externally supplied motions, expressions
and automatic eye blink can be driven through the SDK's own managers.

## Scope

This crate is a rendering and animation primitive binding only. It deliberately
does **not** include:

- playback **policy**: no idle/random motion selection, no skin/param discovery,
  no priority or timeout rules, no expression expiry, no look-at, physics or
  pose playback
- model loading policy, asset databases, `.model3.json` parsing, texture upload
  or atlas management
- window/EGL setup, event loops, or any application/UI logic

The caller owns the EGL context, the current framebuffer, the viewport, the GL
textures and the MVP matrix. The crate owns only native Cubism renderer and
motion state.

### Motion, expression and eye blink

The caller supplies everything the SDK needs; the bridge only forwards:

- **Motion**: `Framework::create_motion` parses external `.motion3.json` bytes,
  `Motion::set_fade_in`/`set_fade_out` set explicit fades, and a
  `MotionManager` started via `Model::motion_manager` plays it with an explicit
  priority (`start(&motion, priority)`), advances with `update(dt)`, and reports
  `is_finished()`, per-entry `entry_is_finished(entry)` or `stop()`.
- **Expression**: `Framework::create_expression` parses external `.exp3.json`
  bytes and an `ExpressionManager` blends it with `start`/`update`.
- **Automatic eye blink**: `Model::eye_blink(&["ParamEyeLOpen", ...])` takes the
  exact model parameter IDs, plus explicit `set_interval`/`set_settings`, and
  `update(dt)` writes the eyelid parameters. IDs are never inferred from
  `.model3.json` groups by this crate.
- **Parameter layering**: `Model::save_parameters`/`load_parameters` expose the
  SDK base-layer snapshot that motion blending expects.

The caller owns the clock and frame order (`LoadParameters` → motion update →
expression/blink → `SaveParameters` → `Model::update` → draw if desired); the
crate never steps time on its own.

## No vendored SDK

The Cubism SDK for Native and GLEW are **not** vendored here. The build requires
authorized external installations:

```sh
CUBISM_SDK_ROOT=/path/to/CubismSdkForNative-5-r.5 \
GLEW_ROOT=/path/to/glew \
CUBISM_TEST_MODEL=/path/to/Samples/Resources/Haru/Haru.model3.json \
LIBGL_ALWAYS_SOFTWARE=1 \
  cargo test --manifest-path rust/cubism-render/Cargo.toml
```

`CUBISM_TEST_MODEL` is a real, externally supplied model; the test fails rather
than silently skipping when the SDK, GLEW, model or a surfaceless EGL desktop
OpenGL implementation is missing. Shaders are read from the SDK's external
`Framework/src/Rendering/OpenGL/Shaders/Standard` directory at runtime.

The real Haru surfaceless test selects a motion group, an expression and the
eye-blink IDs that its `Haru.model3.json` actually declares, steps a fixed `dt`,
and asserts that parameters and rendered pixels change (and that a blink really
closes the eyelids). No sample-specific group name, asset path or absolute host
path is hard-coded.

### Optional proof image

The test writes no file by default. Set `CUBISM_TEST_PROOF_PNG` to a path to keep
the first rendered frame as a PNG; parent directories are created, and a genuine
write failure fails the test instead of being ignored:

```sh
CUBISM_TEST_PROOF_PNG="$PWD/target/cubism-render-proof.png" cargo test ...
```

Note the PNG is a raw GL `glReadPixels` result, so it appears vertically flipped.

## Lifetime rules

- An EGL desktop OpenGL compatibility context must be current on the calling
  thread before `Framework::start`, and must stay current on that same thread
  until every `Framework`, `Model`, `Renderer`, `Motion`, `Expression`,
  `MotionManager`, `ExpressionManager` and `EyeBlink` has been dropped.
- Models retain the `Framework` lease; renderers, motion managers, expression
  managers and eye blinks retain their model, so `Framework::close` fails while
  dependents live and succeeds only afterwards.
- A motion or expression manager keeps its own reference to each started
  motion/expression until that queue entry finishes, is stopped, or the manager
  is dropped. Dropping the caller's handle during playback therefore does not
  invalidate the playing object; the handle's reference is simply released.
- Motion, expression and eye blink handles are additionally `!Send`/`!Sync`.
- GL textures bound to a renderer must outlive it and must not be deleted or
  repurposed while it may draw.

## SDK shutdown quirk

Cubism 5-r.5 shares a single GL program between the Normal/Add/Mult shader sets
but deletes the handle stored in each set on release, so `Dispose` issues
`glDeleteProgram` on already-deleted names and leaves `GL_INVALID_VALUE`
pending. The bridge installs a temporary shim that only deletes programs that
still exist, so shutdown leaves no stale GL error. Real driver failures are not
swallowed: the test asserts `glGetError() == GL_NO_ERROR` after each Dispose.

## 应用侧接线尚未完成

当前 crate 是已验证的渲染与动画基础件，而不是生产桌面渲染路径：运行时仍未使用它，窗口/layer-shell/输入区域以及动画、表情、眨眼策略也尚未从 C++ 迁移。下一步需把此桥接入 Rust runtime 的 EGL 渲染线程，并在调用方实现待机/随机动作、表情超时等策略，再替换现有 Qt 模型呈现路径。

## Licensing

The Cubism SDK is governed by Live2D's own license and is supplied by the user.
This crate contains no SDK source, binaries or model assets.
