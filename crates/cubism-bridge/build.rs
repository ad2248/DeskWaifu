use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

fn required_dir(key: &str, marker: &str) -> PathBuf {
    let path = env::var_os(key)
        .map(PathBuf::from)
        .and_then(|path| path.canonicalize().ok())
        .filter(|path| path.join(marker).is_file());
    path.unwrap_or_else(|| panic!("set {key} to the directory containing {marker}"))
}

fn run(command: &mut Command) {
    let status = command
        .status()
        .unwrap_or_else(|error| panic!("cannot run {command:?}: {error}"));
    assert!(status.success(), "{command:?} failed: {status}");
}

fn main() {
    for path in ["native", "../../thirdParty/stb/stb_image.h"] {
        println!("cargo:rerun-if-changed={path}");
    }
    for key in [
        "CUBISM_SDK_DIR",
        "CUBISM_GLEW_DIR",
        "CUBISM_LAYER_SHELL_XML",
    ] {
        println!("cargo:rerun-if-env-changed={key}");
    }
    if env::var_os("CARGO_FEATURE_NATIVE").is_none() {
        return;
    }
    let target = env::var("TARGET").unwrap();
    assert_eq!(
        target, "x86_64-unknown-linux-gnu",
        "Cubism native PoC supports Linux x86_64 GNU"
    );
    assert_eq!(
        env::var("HOST").unwrap(),
        target,
        "Cubism native PoC requires a native build"
    );

    let core = "Core/lib/linux/x86_64/libLive2DCubismCore.a";
    let sdk = required_dir("CUBISM_SDK_DIR", core);
    let glew = required_dir("CUBISM_GLEW_DIR", "src/glew.c");
    println!("cargo:rerun-if-changed={}", sdk.join(core).display());
    println!("cargo:rerun-if-changed={}", sdk.join("Framework").display());
    println!(
        "cargo:rerun-if-changed={}",
        sdk.join("Core/include").display()
    );
    for path in [glew.join("src/glew.c"), glew.join("include/GL")] {
        println!("cargo:rerun-if-changed={}", path.display());
    }

    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    embed_shaders(&sdk, &out);
    let build = out.join("cmake");
    let window = env::var_os("CARGO_FEATURE_WAYLAND_WINDOW").is_some();
    let protocol = if window {
        let xml = env::var_os("CUBISM_LAYER_SHELL_XML")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                "/usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml".into()
            })
            .canonicalize()
            .expect("set CUBISM_LAYER_SHELL_XML to wlr-layer-shell-unstable-v1.xml");
        println!("cargo:rerun-if-changed={}", xml.display());
        xml
    } else {
        PathBuf::new()
    };
    run(Command::new("cmake")
        .args(["-S", "native", "-B"])
        .arg(&build)
        .arg(format!("-DSDK={}", sdk.display()))
        .arg(format!("-DGLEW={}", glew.display()))
        .arg(format!("-DGENERATED={}", out.display()))
        .arg(format!("-DLAYER_SHELL_XML={}", protocol.display()))
        .args(["-DCMAKE_BUILD_TYPE=Release"]));
    run(Command::new("cmake")
        .arg("--build")
        .arg(&build)
        .args(["--config", "Release", "--target", "dw_cubism", "--parallel"])
        .arg(env::var("NUM_JOBS").unwrap_or_else(|_| "2".into())));

    for path in [build.join("lib"), sdk.join("Core/lib/linux/x86_64")] {
        println!("cargo:rustc-link-search=native={}", path.display());
    }
    for name in ["dw_cubism", "Framework", "dw_glew", "Live2DCubismCore"] {
        println!("cargo:rustc-link-lib=static={name}");
    }
    if window {
        println!("cargo:rustc-link-lib=wayland-egl");
        println!("cargo:rustc-link-lib=wayland-client");
    }
    for name in ["EGL", "OpenGL", "stdc++"] {
        println!("cargo:rustc-link-lib={name}");
    }
}

fn embed_shaders(sdk: &Path, out: &Path) {
    use std::fmt::Write as _;
    let shader_dir = sdk.join("Framework/src/Rendering/OpenGL/Shaders/Standard");
    let mut shaders: Vec<_> = fs::read_dir(shader_dir)
        .expect("Cubism 5-r.5 Standard shaders missing")
        .map(|entry| entry.unwrap().path())
        .filter(|path| {
            matches!(
                path.extension().and_then(|ext| ext.to_str()),
                Some("vert" | "frag")
            )
        })
        .collect();
    shaders.sort();
    let mut header = String::from(
        "#pragma once\n#include <string_view>\n\nstatic std::string_view shader_source(std::string_view path) {\n",
    );
    for path in shaders {
        let name = path.file_name().unwrap().to_str().unwrap();
        let shader = fs::read_to_string(&path).unwrap();
        assert!(!shader.contains(")dwglsl\""));
        assert!(
            name.bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'.')
        );
        writeln!(
            header,
            "    if (path == \"FrameworkShaders/{name}\") return R\"dwglsl({shader})dwglsl\";"
        )
        .unwrap();
    }
    header.push_str("    return {};\n}\n");
    fs::write(out.join("shaders.hpp"), header).unwrap();
}
