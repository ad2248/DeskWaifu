use std::{env, path::PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=CUBISM_SDK_ROOT");
    assert_eq!(
        env::var("CARGO_CFG_TARGET_OS").unwrap(),
        "linux",
        "Cubism Core currently supports Linux only"
    );
    assert_eq!(
        env::var("CARGO_CFG_TARGET_ARCH").unwrap(),
        "x86_64",
        "Cubism Core currently supports x86_64 only"
    );
    let root = PathBuf::from(env::var_os("CUBISM_SDK_ROOT").expect(
        "Set CUBISM_SDK_ROOT to your externally installed Cubism SDK for Native (no SDK is bundled)",
    ));
    let header = root.join("Core/include/Live2DCubismCore.h");
    let library_dir = root.join("Core/lib/linux/x86_64");
    let library = library_dir.join("libLive2DCubismCore.a");
    assert!(header.is_file(), "Missing SDK header: {}", header.display());
    assert!(
        library.is_file(),
        "Missing SDK static library: {}",
        library.display()
    );
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-changed={}", library.display());
    println!("cargo:rustc-link-search=native={}", library_dir.display());
    println!("cargo:rustc-link-lib=static=Live2DCubismCore");
    println!("cargo:rustc-link-lib=dylib=stdc++");
}
