//! Links against the system ONNX Runtime. No runtime is downloaded or bundled:
//! the build only records the linker requirement, so a missing development
//! library fails at link time with a clear error.

fn main() {
    // Honour an explicit search path when one is provided, otherwise rely on
    // the system linker configuration.
    if let Some(path) = std::env::var_os("DESKWAIFU_ONNXRUNTIME_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", path.to_string_lossy());
    }
    println!("cargo:rustc-link-lib=onnxruntime");
    println!("cargo:rerun-if-env-changed=DESKWAIFU_ONNXRUNTIME_LIB_DIR");
    println!("cargo:rerun-if-changed=build.rs");
}
