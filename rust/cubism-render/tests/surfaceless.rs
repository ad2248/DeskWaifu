//! Real external-assets proof; deliberately fails (never silently skips) if its
//! required environment or surfaceless desktop EGL implementation is missing.
use cubism_render::Framework;
use std::{
    ffi::{c_char, c_void, CString},
    path::PathBuf,
    ptr,
};

type Handle = *mut c_void;
#[link(name = "EGL")]
extern "C" {
    fn eglGetPlatformDisplay(platform: u32, native: Handle, attributes: *const isize) -> Handle;
    fn eglInitialize(display: Handle, major: *mut i32, minor: *mut i32) -> u32;
    fn eglBindAPI(api: u32) -> u32;
    fn eglChooseConfig(
        display: Handle,
        attributes: *const i32,
        config: *mut Handle,
        size: i32,
        count: *mut i32,
    ) -> u32;
    fn eglCreatePbufferSurface(display: Handle, config: Handle, attributes: *const i32) -> Handle;
    fn eglCreateContext(
        display: Handle,
        config: Handle,
        shared: Handle,
        attributes: *const i32,
    ) -> Handle;
    fn eglMakeCurrent(display: Handle, draw: Handle, read: Handle, context: Handle) -> u32;
    fn eglGetProcAddress(name: *const c_char) -> *const c_void;
    fn eglDestroyContext(display: Handle, context: Handle) -> u32;
    fn eglDestroySurface(display: Handle, surface: Handle) -> u32;
    fn eglTerminate(display: Handle) -> u32;
}
const SIZE: i32 = 512;
struct Egl {
    display: Handle,
    surface: Handle,
    context: Handle,
}
impl Egl {
    unsafe fn new() -> Self {
        let display = eglGetPlatformDisplay(0x31DD, ptr::null_mut(), ptr::null());
        assert!(
            !display.is_null(),
            "EGL_PLATFORM_SURFACELESS_MESA unavailable"
        );
        assert_ne!(eglInitialize(display, ptr::null_mut(), ptr::null_mut()), 0);
        assert_ne!(eglBindAPI(0x30A2), 0, "desktop OpenGL API unavailable");
        let attrs = [
            0x3033, 1, // EGL_SURFACE_TYPE, EGL_PBUFFER_BIT
            0x3040, 8, // EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT
            0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, // RGBA8
            0x3038,
        ];
        let (mut config, mut count) = (ptr::null_mut(), 0);
        assert_ne!(
            eglChooseConfig(display, attrs.as_ptr(), &mut config, 1, &mut count),
            0
        );
        assert_eq!(count, 1, "RGBA8 pbuffer config unavailable");
        let surface = eglCreatePbufferSurface(
            display,
            config,
            [0x3057, SIZE, 0x3056, SIZE, 0x3038].as_ptr(),
        );
        assert!(!surface.is_null(), "pbuffer creation failed");
        let context = eglCreateContext(display, config, ptr::null_mut(), [0x3038].as_ptr());
        assert!(!context.is_null(), "compatibility context creation failed");
        assert_ne!(eglMakeCurrent(display, surface, surface, context), 0);
        gl::load_with(|name| eglGetProcAddress(CString::new(name).unwrap().as_ptr()));
        Self {
            display,
            surface,
            context,
        }
    }
}
impl Drop for Egl {
    fn drop(&mut self) {
        unsafe {
            eglMakeCurrent(
                self.display,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            );
            eglDestroyContext(self.display, self.context);
            eglDestroySurface(self.display, self.surface);
            eglTerminate(self.display);
        }
    }
}
struct Textures(Vec<u32>);
impl Drop for Textures {
    fn drop(&mut self) {
        unsafe { gl::DeleteTextures(self.0.len() as i32, self.0.as_ptr()) };
    }
}
fn frame(renderer: &mut cubism_render::Renderer, matrix: &[f32; 16]) -> Vec<u8> {
    unsafe {
        gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
        gl::Viewport(0, 0, SIZE, SIZE);
        gl::ClearColor(0., 0., 0., 0.);
        gl::Clear(gl::COLOR_BUFFER_BIT);
    }
    renderer.draw(matrix).unwrap();
    let mut pixels = vec![0; (SIZE * SIZE * 4) as usize];
    unsafe {
        gl::ReadPixels(
            0,
            0,
            SIZE,
            SIZE,
            gl::RGBA,
            gl::UNSIGNED_BYTE,
            pixels.as_mut_ptr().cast(),
        );
        assert_eq!(gl::GetError(), gl::NO_ERROR, "GL error after draw/readback");
    }
    pixels
}

#[test]
fn external_haru_surfaceless_pixels_and_lifetimes() {
    let model_path = PathBuf::from(
        std::env::var_os("CUBISM_TEST_MODEL")
            .expect("set CUBISM_TEST_MODEL to external Haru.model3.json (test is not skipped)"),
    );
    let sdk = PathBuf::from(std::env::var_os("CUBISM_SDK_ROOT").expect("set CUBISM_SDK_ROOT"));
    let shaders = sdk.join("Framework/src/Rendering/OpenGL/Shaders/Standard");
    let root = model_path.parent().unwrap();
    let json: serde_json::Value =
        serde_json::from_slice(&std::fs::read(&model_path).unwrap()).unwrap();
    let files = &json["FileReferences"];
    let moc = std::fs::read(root.join(files["Moc"].as_str().unwrap())).unwrap();
    // Declared first, so context destruction is last even during unwinding.
    let _egl = unsafe { Egl::new() };
    let framework = unsafe { Framework::start(&shaders) }.unwrap();
    assert!(
        unsafe { Framework::start(&shaders) }.is_err(),
        "global owner must be exclusive"
    );
    assert!(framework.create_model(&[]).is_err());
    let model = framework.create_model(&moc).unwrap();
    let mut textures = Textures(Vec::new());
    // Textures outlive the renderer but are still Rust-owned.
    for path in files["Textures"].as_array().unwrap() {
        let image = image::open(root.join(path.as_str().unwrap()))
            .unwrap()
            .into_rgba8();
        let mut texture = 0;
        unsafe {
            gl::GenTextures(1, &mut texture);
            gl::BindTexture(gl::TEXTURE_2D, texture);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR as i32);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR as i32);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE as i32);
            gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE as i32);
            gl::TexImage2D(
                gl::TEXTURE_2D,
                0,
                gl::RGBA8 as i32,
                image.width() as i32,
                image.height() as i32,
                0,
                gl::RGBA,
                gl::UNSIGNED_BYTE,
                image.as_ptr().cast(),
            );
            // The SDK overrides MIN_FILTER with LINEAR_MIPMAP_LINEAR.
            gl::GenerateMipmap(gl::TEXTURE_2D);
            assert_eq!(gl::GetError(), gl::NO_ERROR);
        }
        textures.0.push(texture);
    }
    let mut renderer = model.renderer(SIZE, SIZE).unwrap();
    assert!(renderer.resize(0, SIZE).is_err());
    renderer.resize(SIZE, SIZE).unwrap();
    assert!(renderer.bind_texture(-1, textures.0[0]).is_err());
    for (slot, &texture) in textures.0.iter().enumerate() {
        renderer.bind_texture(slot as i32, texture).unwrap();
    }
    assert!(model.parameter(-1).is_err());
    assert!(model.parameter(model.parameter_count().unwrap()).is_err());
    assert!(model.set_parameter(0, f32::NAN).is_err());
    assert!(model.set_parameter(0, f32::INFINITY).is_err());
    assert!(renderer.draw(&[f32::NAN; 16]).is_err());
    let [width, height] = model.canvas_size().unwrap();
    assert!(width > 0. && height > 0.);
    // Rust owns the complete MVP; fit the full centered canvas with a margin.
    let scale = 1.8 / width.max(height);
    let matrix = [
        scale, 0., 0., 0., 0., scale, 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.,
    ];
    model.update().unwrap();
    let before = frame(&mut renderer, &matrix);
    let pixels = before.as_chunks::<4>().0;
    let colored = pixels.iter().filter(|p| p[3] != 0).count();
    let transparent = pixels.iter().filter(|p| p[3] == 0).count();
    assert!(
        colored > 1000,
        "expected visible model, got {colored} pixels"
    );
    assert!(
        transparent > 1000,
        "expected transparent background, got {transparent} pixels"
    );
    image::save_buffer(
        "/tmp/opencode/cubism-render-proof.png",
        &before,
        SIZE as u32,
        SIZE as u32,
        image::ColorType::Rgba8,
    )
    .unwrap();
    let visible: Vec<_> = pixels.iter().filter(|p| p[3] > 128).collect();
    eprintln!(
        "visible={colored}, transparent={transparent}, samples={:?}",
        &visible[..visible.len().min(10)]
    );
    assert!(
        visible.iter().any(|p| p[..3] != visible[0][..3]),
        "model must have color variation"
    );
    let angle = (0..model.parameter_count().unwrap())
        .find(|&i| model.parameter(i).unwrap().id == "ParamAngleX")
        .expect("Haru ParamAngleX");
    let parameter = model.parameter(angle).unwrap();
    model.set_parameter(angle, parameter.maximum).unwrap();
    assert_eq!(model.parameter(angle).unwrap().value, parameter.maximum);
    model.update().unwrap();
    let after = frame(&mut renderer, &matrix);
    let changed = pixels
        .iter()
        .zip(after.as_chunks::<4>().0)
        .filter(|(a, b)| a != b)
        .count();
    assert!(
        changed > 100,
        "parameter must change rendered pixels, got {changed}"
    );
    eprintln!("Haru proof: visible={colored}, transparent={transparent}, changed={changed}");
    // Closing cannot invalidate retained model/renderer dependencies.
    let framework = match framework.close() {
        Err(f) => f,
        Ok(()) => panic!("closed with live model"),
    };
    drop(model); // renderer's Rc keeps model + Framework alive
    let retained = frame(&mut renderer, &matrix);
    assert_eq!(after, retained);
    drop(renderer);
    drop(textures);
    // Dispose deletes every shader program while EGL is current; the SDK aliases
    // some handles, so shutdown must leave no pending GL error behind.
    assert!(framework.close().is_ok());
    assert_eq!(
        unsafe { gl::GetError() },
        gl::NO_ERROR,
        "GL error after Framework Dispose"
    );
    // A fresh session after Dispose/CleanUp must work and stay error-free.
    let framework = unsafe { Framework::start(&shaders) }.unwrap();
    assert!(framework.close().is_ok());
    assert_eq!(
        unsafe { gl::GetError() },
        gl::NO_ERROR,
        "GL error after second Dispose"
    );
}
