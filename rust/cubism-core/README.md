# Cubism Core Rust boundary

This crate calls the external Live2D Cubism Core **C API** through handwritten, narrow Rust FFI declarations. It does not contain the Cubism SDK or model assets. Set `CUBISM_SDK_ROOT` to an authorized Cubism SDK for Native installation before building; currently supported native target is Linux x86_64.

```sh
CUBISM_SDK_ROOT=/path/to/CubismSdkForNative-5-r.5 \
  CUBISM_TEST_MOC=/path/to/Samples/Resources/Haru/Haru.moc3 \
  cargo test --manifest-path rust/cubism-core/Cargo.toml
```

`Moc` and `Model` own Core's aligned in-place memory and remain thread-affine (`!Send`/`!Sync`). They do not initialize Cubism Framework, OpenGL, or a renderer. That work must be performed by the future render-thread/graphics bridge while preserving the existing GL-context ownership and shutdown ordering. This Core-only crate is not yet wired into the shipping desktop process.
