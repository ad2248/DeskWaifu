use std::{env, path::PathBuf};

fn main() {
    for key in ["CUBISM_SDK_ROOT", "GLEW_ROOT"] {
        println!("cargo:rerun-if-env-changed={key}");
    }
    println!("cargo:rerun-if-changed=native");
    let sdk = PathBuf::from(env::var_os("CUBISM_SDK_ROOT").expect("set CUBISM_SDK_ROOT"));
    let glew = PathBuf::from(env::var_os("GLEW_ROOT").expect("set GLEW_ROOT"));
    let dst = cmake::Config::new("native")
        .define("CUBISM_SDK_ROOT", &sdk)
        .define("GLEW_ROOT", glew)
        .profile("Release")
        .build();
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!(
        "cargo:rustc-link-search=native={}/Core/lib/linux/x86_64",
        sdk.display()
    );
    for lib in ["cubism_bridge", "Framework", "Live2DCubismCore", "GLEW"] {
        println!("cargo:rustc-link-lib=static={lib}");
    }
    for lib in ["stdc++", "GL", "EGL"] {
        println!("cargo:rustc-link-lib={lib}");
    }
}
