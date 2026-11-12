# cubism-render

Narrow Rust facade over the external Live2D Cubism Framework **OpenGL renderer**.
It proves that a moc3 model can be loaded, parameterised and drawn into a
caller-owned GL framebuffer, and nothing more.

## Scope

This crate is a renderer binding only. It deliberately does **not** include:

- animation, motion, physics, pose, expression or look-at playback
- model loading policy, asset databases, texture upload or atlas management
- window/EGL setup, event loops, or any application/UI logic

The caller owns the EGL context, the current framebuffer, the viewport, the GL
textures and the MVP matrix. The crate owns only native Cubism renderer state.

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

## Lifetime rules

- An EGL desktop OpenGL compatibility context must be current on the calling
  thread before `Framework::start`, and must stay current on that same thread
  until every `Framework`, `Model` and `Renderer` clone has been dropped.
- Models retain the `Framework` lease; renderers retain their model, so
  `Framework::close` fails while dependents live and succeeds only afterwards.
- GL textures bound to a renderer must outlive it and must not be deleted or
  repurposed while it may draw.
- All handles are `!Send`/`!Sync`.

## SDK shutdown quirk

Cubism 5-r.5 shares a single GL program between the Normal/Add/Mult shader sets
but deletes the handle stored in each set on release, so `Dispose` issues
`glDeleteProgram` on already-deleted names and leaves `GL_INVALID_VALUE`
pending. The bridge installs a temporary shim that only deletes programs that
still exist, so shutdown leaves no stale GL error. Real driver failures are not
swallowed: the test asserts `glGetError() == GL_NO_ERROR` after each Dispose.

## 应用侧接线尚未完成

当前 crate 是已验证的渲染基础件，而不是生产桌面渲染路径：运行时仍未使用它，窗口/layer-shell/输入区域和动画策略也尚未从 C++ 迁移。下一步需把此桥接入 Rust runtime 的 EGL 渲染线程，再替换现有 Qt 模型呈现路径。

## Licensing

The Cubism SDK is governed by Live2D's own license and is supplied by the user.
This crate contains no SDK source, binaries or model assets.
