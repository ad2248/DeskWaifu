//! Real external-assets proof; deliberately fails (never silently skips) if its
//! required environment or surfaceless desktop EGL implementation is missing.
use cubism_render::{Framework, Model, MotionManager};
use std::{
    ffi::{c_char, c_void, CString},
    path::{Path, PathBuf},
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
/// The native Framework lease is process-global, so parallel test threads must
/// not start two sessions. Each test holds this for its whole lifetime.
static SESSION: std::sync::Mutex<()> = std::sync::Mutex::new(());
fn session() -> std::sync::MutexGuard<'static, ()> {
    // A panicking test still unwinds its Framework, so the poisoned lock is safe
    // to reuse; refusing to run would only hide the original failure.
    SESSION
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
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
/// Writes a proof PNG only when the caller asks for one. Nothing is written to a
/// fixed host path by default, and a requested-but-unwritable file fails loudly.
fn save_proof(pixels: &[u8]) {
    let Some(path) = std::env::var_os("CUBISM_TEST_PROOF_PNG") else {
        return;
    };
    let path = PathBuf::from(path);
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)
            .unwrap_or_else(|e| panic!("cannot create proof directory {}: {e}", parent.display()));
    }
    image::save_buffer(
        &path,
        pixels,
        SIZE as u32,
        SIZE as u32,
        image::ColorType::Rgba8,
    )
    .unwrap_or_else(|e| panic!("cannot write proof image {}: {e}", path.display()));
    eprintln!("proof image: {}", path.display());
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
/// One expression declared by the model3 json, with its targeted parameters.
struct ExpressionFixture {
    name: String,
    bytes: Vec<u8>,
    targets: Vec<String>,
}
/// External Haru location plus the parts of its model3 json the tests select.
struct Haru {
    shaders: PathBuf,
    moc: Vec<u8>,
    textures: Vec<Vec<u8>>,
    motion_group: String,
    motion: Vec<u8>,
    expressions: Vec<ExpressionFixture>,
    blink_ids: Vec<String>,
}
fn json(path: &Path) -> serde_json::Value {
    serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap()
}
fn load() -> Haru {
    let sdk = PathBuf::from(std::env::var_os("CUBISM_SDK_ROOT").expect("set CUBISM_SDK_ROOT"));
    let model_path = PathBuf::from(
        std::env::var_os("CUBISM_TEST_MODEL")
            .expect("set CUBISM_TEST_MODEL to external Haru.model3.json (test is not skipped)"),
    );
    let root = model_path.parent().unwrap();
    let model = json(&model_path);
    let files = &model["FileReferences"];
    let moc = std::fs::read(root.join(files["Moc"].as_str().unwrap())).unwrap();
    let textures = files["Textures"]
        .as_array()
        .unwrap()
        .iter()
        .map(|path| std::fs::read(root.join(path.as_str().unwrap())).unwrap())
        .collect();
    // A motion declared by the model3 json; no sample-specific group is hard-coded.
    let (motion_group, motion_file) = files["Motions"]
        .as_object()
        .unwrap()
        .iter()
        .find_map(|(group, motions)| {
            let file = motions.as_array()?.first()?.get("File")?.as_str()?;
            Some((group.clone(), root.join(file)))
        })
        .expect("model3 declares at least one motion");
    let motion = std::fs::read(&motion_file).unwrap();
    // Every expression declared by the model3 json, so the test can pick one that
    // is actually observable instead of assuming the first entry bends something.
    let expressions: Vec<ExpressionFixture> = files["Expressions"]
        .as_array()
        .expect("model3 declares expressions")
        .iter()
        .map(|entry| {
            let name = entry["Name"].as_str().unwrap().to_owned();
            let file = root.join(entry["File"].as_str().unwrap());
            let targets = json(&file)["Parameters"]
                .as_array()
                .expect("exp3.json declares parameters")
                .iter()
                .map(|parameter| parameter["Id"].as_str().unwrap().to_owned())
                .collect();
            ExpressionFixture {
                name,
                bytes: std::fs::read(&file).unwrap(),
                targets,
            }
        })
        .collect();
    // Eye blink groups are declared by the model3 json; nothing is inferred.
    let blink_ids = model["Groups"]
        .as_array()
        .unwrap()
        .iter()
        .find(|group| group["Name"] == "EyeBlink")
        .and_then(|group| group["Ids"].as_array())
        .expect("model3 declares an EyeBlink group")
        .iter()
        .map(|id| id.as_str().unwrap().to_owned())
        .collect();
    Haru {
        shaders: sdk.join("Framework/src/Rendering/OpenGL/Shaders/Standard"),
        moc,
        textures,
        motion_group,
        motion,
        expressions,
        blink_ids,
    }
}
impl Haru {
    fn attach(&self, model: &Model) -> (cubism_render::Renderer, Textures, [f32; 16]) {
        let mut textures = Textures(Vec::new());
        // Textures outlive the renderer but are still Rust-owned.
        for bytes in &self.textures {
            let image = image::load_from_memory(bytes).unwrap().into_rgba8();
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
        for (slot, &texture) in textures.0.iter().enumerate() {
            renderer.bind_texture(slot as i32, texture).unwrap();
        }
        // Rust owns the complete MVP; fit the full centered canvas with a margin.
        let [width, height] = model.canvas_size().unwrap();
        let scale = 1.8 / width.max(height);
        let matrix = [
            scale, 0., 0., 0., 0., scale, 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.,
        ];
        (renderer, textures, matrix)
    }
}
fn parameter_index(model: &Model, id: &str) -> i32 {
    (0..model.parameter_count().unwrap())
        .find(|&i| model.parameter(i).unwrap().id == id)
        .unwrap_or_else(|| panic!("Haru {id}"))
}
fn parameter_value(model: &Model, id: &str) -> f32 {
    model.parameter(parameter_index(model, id)).unwrap().value
}
/// Steps a manager over a deterministic frame clock.
fn step(manager: &mut MotionManager, frames: usize, dt: f32) -> bool {
    let mut updated = false;
    for _ in 0..frames {
        updated |= manager.update(dt).unwrap();
    }
    updated
}
fn changed_pixels(left: &[u8], right: &[u8]) -> usize {
    left.as_chunks::<4>()
        .0
        .iter()
        .zip(right.as_chunks::<4>().0)
        .filter(|(a, b)| a != b)
        .count()
}

#[test]
fn external_haru_surfaceless_pixels_and_lifetimes() {
    let _session = session();
    let haru = load();
    let shaders = haru.shaders.clone();
    // Declared first, so context destruction is last even during unwinding.
    let _egl = unsafe { Egl::new() };
    let framework = unsafe { Framework::start(&shaders) }.unwrap();
    assert!(
        unsafe { Framework::start(&shaders) }.is_err(),
        "global owner must be exclusive"
    );
    assert!(framework.create_model(&[]).is_err());
    assert!(framework.create_motion(&[]).is_err());
    assert!(framework.create_expression(&[]).is_err());
    assert!(
        framework.create_motion(b"not json").is_err(),
        "malformed motion json must fail with a clear error"
    );
    assert!(
        framework
            .create_motion(b"{\"Version\":3,\"Meta\":{}}")
            .is_err(),
        "motion without curves must fail with a clear error"
    );
    assert!(
        framework.create_expression(b"not json").is_err(),
        "malformed expression json must fail with a clear error"
    );
    assert!(
        framework
            .create_expression(b"{\"Type\":\"Live2D Expression\",\"Parameters\":[]}")
            .is_err(),
        "expression without parameters must fail with a clear error"
    );
    assert!(
        framework.create_expression(b"not json").is_err(),
        "malformed expression json must fail with a clear error"
    );
    let model = framework.create_model(&haru.moc).unwrap();
    assert!(model.eye_blink(&[]).is_err());
    assert!(
        model.eye_blink(&["ParamNotOnHaru"]).is_err(),
        "eye blink IDs must exist on the model"
    );
    let (mut renderer, textures, matrix) = haru.attach(&model);
    assert!(renderer.resize(0, SIZE).is_err());
    renderer.resize(SIZE, SIZE).unwrap();
    assert!(renderer.bind_texture(-1, textures.0[0]).is_err());
    assert!(model.parameter(-1).is_err());
    assert!(model.parameter(model.parameter_count().unwrap()).is_err());
    assert!(model.set_parameter(0, f32::NAN).is_err());
    assert!(model.set_parameter(0, f32::INFINITY).is_err());
    assert!(renderer.draw(&[f32::NAN; 16]).is_err());
    let [width, height] = model.canvas_size().unwrap();
    assert!(width > 0. && height > 0.);
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
    // Optional artifact only; the pixel assertions above do not depend on it.
    save_proof(&before);
    let visible: Vec<_> = pixels.iter().filter(|p| p[3] > 128).collect();
    eprintln!(
        "visible={colored}, transparent={transparent}, samples={:?}",
        &visible[..visible.len().min(10)]
    );
    assert!(
        visible.iter().any(|p| p[..3] != visible[0][..3]),
        "model must have color variation"
    );
    let angle = parameter_index(&model, "ParamAngleX");
    let parameter = model.parameter(angle).unwrap();
    model.set_parameter(angle, parameter.maximum).unwrap();
    assert_eq!(model.parameter(angle).unwrap().value, parameter.maximum);
    model.update().unwrap();
    let after = frame(&mut renderer, &matrix);
    let changed = changed_pixels(&before, &after);
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

#[test]
fn a_motion_with_a_user_data_event_does_not_crash_the_motion_manager() {
    // Regression: `CubismMotionQueueManager::UpdateMotion` calls its event
    // callback for every fired `UserData` event without a null check, and the
    // SDK default is null. Starting a motion with `UserData` therefore killed
    // the process with SIGSEGV instead of playing.
    let _session = session();
    let haru = load();
    let _egl = unsafe { Egl::new() };
    let framework = unsafe { Framework::start(&haru.shaders) }.unwrap();
    let model = framework.create_model(&haru.moc).unwrap();
    let mut document: serde_json::Value = serde_json::from_slice(&haru.motion).unwrap();
    document["Meta"]["UserDataCount"] = serde_json::json!(1);
    document["Meta"]["TotalUserDataSize"] = serde_json::json!(4);
    document["UserData"] = serde_json::json!([{"Time": 0.2, "Value": "test"}]);
    let motion_json = serde_json::to_vec_pretty(&document).unwrap();
    let motion = framework.create_motion(&motion_json).unwrap();
    let mut manager = model.motion_manager().unwrap();
    model.save_parameters().unwrap();
    manager.start(&motion, 0).unwrap();
    // Step past the event time; the callback must at least be installed.
    assert!(step(&mut manager, 10, 0.05), "motion should update");
    assert!(manager.update(0.0).is_ok());
}

#[test]
fn external_haru_motion_expression_eye_blink_surfaceless() {
    let _session = session();
    let haru = load();
    let _egl = unsafe { Egl::new() };
    let framework = unsafe { Framework::start(&haru.shaders) }.unwrap();
    let model = framework.create_model(&haru.moc).unwrap();
    let (mut renderer, textures, matrix) = haru.attach(&model);

    // --- motion: external bytes, explicit fade/priority, deterministic dt ---
    let mut motion = framework.create_motion(&haru.motion).unwrap();
    assert!(motion.set_fade_in(-1.0).is_err());
    assert!(motion.set_fade_out(f32::NAN).is_err());
    motion.set_fade_in(0.0).unwrap();
    motion.set_fade_out(0.25).unwrap();
    let mut manager = model.motion_manager().unwrap();
    assert!(manager.update(-0.1).is_err());
    assert!(manager.update(f32::NAN).is_err());
    assert!(manager.is_finished().unwrap(), "no motion started yet");
    let entry = manager.start(&motion, 0).unwrap();
    assert!(!manager.entry_is_finished(entry).unwrap());
    // Motion blending reads the saved base layer, as the SDK's model loop expects.
    model.save_parameters().unwrap();
    let mut watched: Vec<(String, f32)> = (0..model.parameter_count().unwrap())
        .map(|i| {
            let parameter = model.parameter(i).unwrap();
            (parameter.id, parameter.value)
        })
        .collect();
    let resting = frame(&mut renderer, &matrix);
    // Step far enough into the (looping) motion for curves to diverge from t=0.
    assert!(step(&mut manager, 30, 0.05), "motion should update");
    model.update().unwrap();
    let moved: Vec<f32> = watched
        .iter()
        .map(|(id, _)| parameter_value(&model, id))
        .collect();
    let changed_parameters = watched
        .iter()
        .zip(&moved)
        .filter(|((_, before), after)| *before != **after)
        .count();
    assert!(
        changed_parameters > 0,
        "motion group {} must change parameters, got {changed_parameters}",
        haru.motion_group
    );
    let moved_pixels = frame(&mut renderer, &matrix);
    let changed = changed_pixels(&resting, &moved_pixels);
    assert!(
        changed > 100,
        "motion must change rendered pixels, got {changed}"
    );
    eprintln!(
        "motion group={} parameters={changed_parameters} pixels={changed}",
        haru.motion_group
    );
    // LoadParameters restores the base layer captured before playback.
    model.load_parameters().unwrap();
    watched.iter_mut().for_each(|(id, before)| {
        assert_eq!(
            *before,
            parameter_value(&model, id),
            "LoadParameters for {id}"
        );
    });
    // Dropping the caller's motion handle mid-playback must not stop playback:
    // the manager holds its own reference until the entry goes away.
    drop(motion);
    assert!(!manager.entry_is_finished(entry).unwrap());
    assert!(step(&mut manager, 10, 0.05), "motion survives handle drop");
    // Stopping releases the manager's reference and stops reporting the entry.
    manager.stop().unwrap();
    assert!(manager.is_finished().unwrap());
    assert!(manager.entry_is_finished(entry).unwrap());
    assert!(
        framework.create_motion(b"not json").is_err(),
        "malformed motion json must fail with a clear error"
    );
    assert!(
        framework
            .create_motion(b"{\"Version\":3,\"Meta\":{}}")
            .is_err(),
        "motion without curves must fail with a clear error"
    );
    assert!(
        framework.create_expression(b"not json").is_err(),
        "malformed expression json must fail with a clear error"
    );
    assert!(
        framework
            .create_expression(b"{\"Type\":\"Live2D Expression\",\"Parameters\":[]}")
            .is_err(),
        "expression without parameters must fail with a clear error"
    );

    // --- expression: external bytes blended onto the base layer ---
    // Some sample expressions push a parameter that is already clamped (Haru's
    // first one adds to a ParamMouthForm pinned at 1.0), so try each declared
    // expression with a fresh manager and require at least one observable change.
    let mut observed = None;
    for fixture in &haru.expressions {
        let expression = framework.create_expression(&fixture.bytes).unwrap();
        let mut expressions = model.expression_manager().unwrap();
        assert!(expressions.update(-1.0).is_err());
        model.load_parameters().unwrap();
        expressions.start(&expression).unwrap();
        let before: Vec<f32> = fixture
            .targets
            .iter()
            .map(|id| parameter_value(&model, id))
            .collect();
        // The default expression fade is one second; step past it deterministically.
        for _ in 0..30 {
            assert!(expressions.update(0.05).unwrap());
        }
        let after: Vec<f32> = fixture
            .targets
            .iter()
            .map(|id| parameter_value(&model, id))
            .collect();
        eprintln!(
            "expression {} targets={:?} before={before:?} after={after:?}",
            fixture.name, fixture.targets
        );
        if before.iter().zip(&after).any(|(a, b)| a != b) {
            observed = Some(fixture.name.clone());
            break;
        }
    }
    assert!(
        observed.is_some(),
        "no declared expression changed its targeted parameters"
    );
    // As with motions, a manager keeps its own reference: dropping the caller's
    // expression handle mid-blend must not invalidate the running expression.
    let expression = framework
        .create_expression(&haru.expressions[0].bytes)
        .unwrap();
    let mut expressions = model.expression_manager().unwrap();
    model.load_parameters().unwrap();
    expressions.start(&expression).unwrap();
    drop(expression);
    assert!(
        expressions.update(0.05).unwrap(),
        "expression survives handle drop"
    );

    // --- automatic eye blink via explicit parameter IDs ---
    model.load_parameters().unwrap();
    let ids: Vec<&str> = haru.blink_ids.iter().map(String::as_str).collect();
    let mut blink = model.eye_blink(&ids).unwrap();
    assert!(blink.update(-1.0).is_err());
    assert!(blink.set_interval(-0.5).is_err());
    assert!(blink.set_settings(0.0, 0.05, 0.1).is_err());
    assert!(blink.set_settings(f32::NAN, 0.05, 0.1).is_err());
    // A short deterministic cadence: the SDK picks an interval in [0, 2*interval),
    // so 0.5 s makes the first blink start on the second step.
    blink.set_interval(0.5).unwrap();
    blink.set_settings(0.05, 0.05, 0.1).unwrap();
    let open: Vec<f32> = haru
        .blink_ids
        .iter()
        .map(|id| parameter_value(&model, id))
        .collect();
    let mut minimum: Vec<f32> = open.clone();
    for _ in 0..200 {
        blink.update(0.01).unwrap();
        for (i, id) in haru.blink_ids.iter().enumerate() {
            minimum[i] = minimum[i].min(parameter_value(&model, id));
        }
    }
    assert!(
        minimum
            .iter()
            .zip(&open)
            .all(|(min, open)| *min < *open - 0.5),
        "eye blink must close {ids:?}: open={open:?} minimum={minimum:?}"
    );
    eprintln!("eye blink open={open:?} minimum={minimum:?}");
    model.update().unwrap();
    frame(&mut renderer, &matrix);

    // Every dependent handle must go before Framework::close succeeds.
    let framework = match framework.close() {
        Err(framework) => framework,
        Ok(()) => panic!("closed with live motion/expression/blink handles"),
    };
    drop(blink);
    drop(expressions);
    drop(manager);
    drop(renderer);
    drop(model);
    drop(textures);
    assert!(framework.close().is_ok());
    assert_eq!(
        unsafe { gl::GetError() },
        gl::NO_ERROR,
        "GL error after motion/expression/blink Dispose"
    );
}
