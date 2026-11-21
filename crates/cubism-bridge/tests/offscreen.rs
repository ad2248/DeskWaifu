#![cfg(feature = "native")]

use std::sync::Mutex;

// Cubism Framework 在同一进程中只有一个实例。
static NATIVE_TEST: Mutex<()> = Mutex::new(());

#[test]
#[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
fn toggle_layer_survives_ordinary_expression_timeout() {
    use cubism_bridge::{Runtime, rgba_len};
    use std::{env, path::Path};

    let _serial = NATIVE_TEST.lock().unwrap();
    let sdk = env::var_os("CUBISM_SDK_DIR").expect("set CUBISM_SDK_DIR");
    let model_path = Path::new(&sdk).join("Samples/Resources/Haru/Haru.model3.json");
    let runtime = Runtime::new(256, 256).unwrap();
    let mut model = runtime.load(&model_path).unwrap();
    let expression = Path::new("expressions/F04.exp3.json");
    let mut pixels = vec![0; rgba_len(256, 256).unwrap()];
    // 让普通表情先超时，确认持久开关仍然生效，再推进到开关期限并检查状态。
    model.set_expression_timeout(Some(0.1)).unwrap();
    model.play_expression("F02").unwrap();
    assert!(
        model
            .toggle_expression(expression, 0.05, Some(0.5))
            .unwrap()
    );
    for _ in 0..3 {
        model.render(0.1, &mut pixels).unwrap();
    }
    assert!(model.expression_toggle_active(expression).unwrap());
    for _ in 0..4 {
        model.render(0.1, &mut pixels).unwrap();
    }
    assert!(!model.expression_toggle_active(expression).unwrap());
    assert!(model.toggle_expression(expression, 0.0, None).unwrap());
    model.reset_expression_toggles();
    assert!(!model.expression_toggle_active(expression).unwrap());
}

#[test]
#[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
fn renders_sample_and_survives_failed_reload() {
    use cubism_bridge::{Runtime, rgba_len};
    use std::{env, path::Path};

    let _serial = NATIVE_TEST.lock().unwrap();
    let sdk = env::var_os("CUBISM_SDK_DIR").expect("set CUBISM_SDK_DIR");
    let json = Path::new(&sdk).join("Samples/Resources/Haru/Haru.model3.json");
    // 通过同线程重建运行时验证全局 Framework 及 EGL 能完整析构。
    for _ in 0..2 {
        let runtime = Runtime::new(256, 256).unwrap();
        assert!(Runtime::new(256, 256).is_err());
        let mut model = runtime.load(&json).unwrap();
        // 确保测试样例确实启用了 Cubism 遮罩路径。
        assert!(model.uses_masking());
        let mut still = vec![0; rgba_len(256, 256).unwrap()];
        model.render(0.0, &mut still).unwrap();
        // 检查透明背景和不透明主体，再对比动作前后的实际像素。
        assert!(still.chunks_exact(4).any(|p| p[3] == 0));
        // 要求纹理产生彩色像素，防止缺少 mipmap 时只画出黑色轮廓也能通过。
        assert!(still.chunks_exact(4).any(|p| p[3] == 255 && p[0] != p[1]));
        assert!(model.render(f32::NAN, &mut still).is_err());
        assert!(model.render(0.0, &mut []).is_err());
        assert!(
            runtime
                .load(&json.with_file_name("missing.model3.json"))
                .is_err()
        );
        model.play_motion("Idle", 1).unwrap();
        let mut animated = still.clone();
        for _ in 0..40 {
            model.render(1.0 / 60.0, &mut animated).unwrap();
        }
        let changed = still
            .chunks_exact(4)
            .zip(animated.chunks_exact(4))
            .filter(|(a, b)| a != b)
            .count();
        assert!(changed > 200, "only {changed} pixels changed");
        assert!(model.motion_groups().unwrap().contains(&("Idle".into(), 2)));
        drop(model);

        // 重新加载 Haru，从无动作的初始状态验证表情像素。
        let mut haru = runtime.load(&json).unwrap();
        assert!(haru.expressions().unwrap().contains(&"F01".to_owned()));
        assert!(haru.play_expression("missing").is_err());
        haru.render(0.0, &mut still).unwrap();
        haru.play_expression("F04").unwrap();
        for _ in 0..40 {
            haru.render(1.0 / 60.0, &mut animated).unwrap();
        }
        assert!(
            still != animated,
            "Haru F04 did not change the rendered frame"
        );
    }
}

#[test]
#[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
fn look_and_lip_sync_compose_with_animation_and_reset() {
    use cubism_bridge::{Model, Runtime, rgba_len};
    use std::path::PathBuf;

    const SIDE: u32 = 256;
    let _serial = NATIVE_TEST.lock().unwrap();
    let resources = PathBuf::from(std::env::var_os("CUBISM_SDK_DIR").expect("set CUBISM_SDK_DIR"))
        .join("Samples/Resources");
    let haru_json = resources.join("Haru/Haru.model3.json");
    let runtime = Runtime::new(SIDE, SIDE).unwrap();
    let mut haru = runtime.load(&haru_json).unwrap();
    let frame = |model: &mut Model<'_>, dt| {
        let mut pixels = vec![0; rgba_len(SIDE, SIDE).unwrap()];
        model.render(dt, &mut pixels).unwrap();
        pixels
    };
    let changed = |left: &[u8], right: &[u8]| {
        left.chunks_exact(4)
            .zip(right.chunks_exact(4))
            .filter(|(a, b)| a != b)
            .count()
    };

    // 固定帧时间，单独比较嘴部和视线变化、连续帧不累积以及清除后回到基线。
    let neutral = frame(&mut haru, 0.0);
    haru.set_lip_sync(Some(0.85)).unwrap();
    let talking = frame(&mut haru, 0.0);
    assert!(changed(&neutral, &talking) > 10);
    for invalid in [f32::NAN, f32::INFINITY, -0.01, 1.01] {
        assert!(haru.set_lip_sync(Some(invalid)).is_err());
    }
    assert_eq!(changed(&talking, &frame(&mut haru, 0.0)), 0);
    haru.set_lip_sync(None).unwrap();
    assert_eq!(changed(&neutral, &frame(&mut haru, 0.0)), 0);

    haru.set_look(Some((0.65, -0.45))).unwrap();
    let gaze = frame(&mut haru, 0.0);
    assert!(changed(&neutral, &gaze) > 200);
    for invalid in [
        (f32::NAN, 0.0),
        (0.0, f32::NEG_INFINITY),
        (1.01, 0.0),
        (0.0, -1.01),
    ] {
        assert!(haru.set_look(Some(invalid)).is_err());
    }
    assert_eq!(changed(&gaze, &frame(&mut haru, 0.0)), 0);
    haru.set_look(Some((-0.65, 0.45))).unwrap();
    assert!(changed(&gaze, &frame(&mut haru, 0.0)) > 200);
    haru.set_look(None).unwrap();
    assert_eq!(changed(&neutral, &frame(&mut haru, 0.0)), 0);

    // 动作与张嘴表情同时运行。零视线不覆盖动作，显式闭嘴覆盖表情，清除后恢复原帧。
    haru.play_motion("TapBody", 0).unwrap();
    haru.play_expression("F02").unwrap();
    frame(&mut haru, 0.0);
    let animated = frame(&mut haru, 0.7);
    assert!(changed(&neutral, &animated) > 200);
    haru.set_look(Some((0.0, 0.0))).unwrap();
    assert_eq!(changed(&animated, &frame(&mut haru, 0.0)), 0);
    haru.set_lip_sync(Some(0.0)).unwrap();
    assert!(changed(&animated, &frame(&mut haru, 0.0)) > 10);
    haru.set_look(Some((0.65, -0.45))).unwrap();
    assert!(changed(&animated, &frame(&mut haru, 0.0)) > 200);
    haru.set_lip_sync(None).unwrap();
    haru.set_look(None).unwrap();
    assert_eq!(changed(&animated, &frame(&mut haru, 0.0)), 0);

    // Haru 本身有可驱动的嘴部参数。将 LipSync 组置空后，验证不会回退到该参数。
    let fixture = HaruFixture::new();
    let empty_lip_sync_path = fixture.model(
        "empty-lip-sync.model3.json",
        "",
        r#", "Groups":[{"Target":"Parameter","Name":"LipSync","Ids":[]}]"#,
    );
    let mut empty_lip_sync = runtime.load(&empty_lip_sync_path).unwrap();
    assert!(empty_lip_sync.expressions().unwrap().is_empty());
    let unchanged = frame(&mut empty_lip_sync, 0.0);
    empty_lip_sync.set_lip_sync(Some(1.0)).unwrap();
    assert_eq!(unchanged, frame(&mut empty_lip_sync, 0.0));
}

struct HaruFixture(std::path::PathBuf);

impl HaruFixture {
    fn new() -> Self {
        let dir = std::env::temp_dir().join(format!(
            "dw-cubism-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&dir).unwrap();
        let fixture = Self(dir);
        let haru = std::path::PathBuf::from(std::env::var_os("CUBISM_SDK_DIR").unwrap())
            .join("Samples/Resources/Haru");
        for name in ["Haru.moc3", "Haru.2048", "expressions"] {
            std::os::unix::fs::symlink(haru.join(name), fixture.0.join(name)).unwrap();
        }
        fixture
    }

    fn model(&self, name: &str, refs: &str, extra: &str) -> std::path::PathBuf {
        let path = self.0.join(name);
        std::fs::write(
            &path,
            format!(
                r#"{{"Version":3,"FileReferences":{{"Moc":"Haru.moc3","Textures":["Haru.2048/texture_00.png","Haru.2048/texture_01.png"]{refs}}}{extra}}}"#
            ),
        )
        .unwrap();
        path
    }

    fn motion(&self, name: &str, target: &str, id: &str, value: f32) {
        std::fs::write(
            self.0.join(name),
            format!(
                // Cubism 5-r.5's parser expects a newline after a final numeric value.
                r#"{{
                    "Version": 3,
                    "Meta": {{
                        "Duration": 2, "Fps": 30, "Loop": false, "AreBeziersRestricted": true,
                        "CurveCount": 1, "TotalSegmentCount": 1, "TotalPointCount": 2,
                        "UserDataCount": 0, "TotalUserDataSize": 0,
                        "FadeInTime": 0, "FadeOutTime": 1
                    }},
                    "Curves": [{{"Target": "{target}", "Id": "{id}", "Segments": [
                        0, {value}, 0, 2, {value}
                    ]}}]
                }}"#
            ),
        )
        .unwrap();
    }
}

impl Drop for HaruFixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

const TEST_WIDTH: u32 = 384;
const TEST_HEIGHT: u32 = 216;

fn capture(model: &mut cubism_bridge::Model<'_>, dt: f32) -> Vec<u8> {
    let mut frame = vec![0; cubism_bridge::rgba_len(TEST_WIDTH, TEST_HEIGHT).unwrap()];
    model.render(dt, &mut frame).unwrap();
    frame
}

fn settle(model: &mut cubism_bridge::Model<'_>) -> Vec<u8> {
    capture(model, 0.0);
    capture(model, 0.7)
}

#[test]
#[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
fn skin_cycles_groups_resets_previous_curves_and_falls_back_to_expressions() {
    use cubism_bridge::Runtime;

    let _serial = NATIVE_TEST.lock().unwrap();
    let fixture = HaruFixture::new();
    fixture.motion("a.motion3.json", "Parameter", "ParamAngleX", 22.0);
    fixture.motion("b.motion3.json", "Parameter", "ParamAngleY", -18.0);
    let skin_path = fixture.model(
        "skins.model3.json",
        r#", "Motions":{"A":[{"File":"a.motion3.json"}],"B":[{"File":"b.motion3.json"}]}"#,
        "",
    );
    let runtime = Runtime::new(TEST_WIDTH, TEST_HEIGHT).unwrap();
    let mut switched = runtime.load(&skin_path).unwrap();
    let neutral = capture(&mut switched, 0.0);
    let expected = |group| {
        let mut model = runtime.load(&skin_path).unwrap();
        model.play_motion(group, 0).unwrap();
        settle(&mut model)
    };
    let a = expected("A");
    let b = expected("B");
    assert!(neutral != a && neutral != b && a != b);
    // 首次切换选第二组，下一次回到第一组。与干净模型的像素比较，验证前一组独占参数已恢复默认值。
    switched.switch_skin().unwrap();
    assert!(settle(&mut switched) == b, "first switch should start B");
    // 第二个句柄从自己的初始索引启动。
    let mut other = runtime.load(&skin_path).unwrap();
    other.switch_skin().unwrap();
    assert!(settle(&mut other) == b, "skin cursor must be model-local");
    switched.switch_skin().unwrap();
    assert!(settle(&mut switched) == a, "B's angle must reset before A");

    let expression_path = fixture.model(
        "expressions.model3.json",
        r#", "Expressions":[{"Name":"F04","File":"expressions/F04.exp3.json"}]"#,
        "",
    );
    let mut expressions = runtime.load(&expression_path).unwrap();
    expressions.switch_skin().unwrap();
    let mut direct_expression = runtime.load(&expression_path).unwrap();
    direct_expression.play_expression("F04").unwrap();
    let expression = settle(&mut direct_expression);
    assert!(expression != neutral);
    assert!(settle(&mut expressions) == expression);
    let mut bare = runtime
        .load(&fixture.model("bare.model3.json", "", ""))
        .unwrap();
    bare.switch_skin().unwrap();
    assert!(settle(&mut bare) == neutral);
}

#[test]
#[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
fn tap_maps_top_down_hit_areas_and_preserves_legacy_fallback() {
    use cubism_bridge::{Runtime, TapHit};

    let _serial = NATIVE_TEST.lock().unwrap();
    let fixture = HaruFixture::new();
    fixture.motion("body.motion3.json", "Parameter", "ParamAngleX", 22.0);
    let refs = r#", "Motions":{"TapBody":[{"File":"body.motion3.json"}]},"Expressions":[{"Name":"F04","File":"expressions/F04.exp3.json"}]"#;
    let hit_areas = r#", "Layout":{"x":0.48,"y":-0.24
    }, "HitAreas":[{"Name":"Body","Id":"HitArea2"},{"Name":"Body","Id":"HitArea"},{"Name":"Head","Id":"HitArea"}]"#;
    let path = fixture.model("tap.model3.json", refs, hit_areas);
    let runtime = Runtime::new(TEST_WIDTH, TEST_HEIGHT).unwrap();
    let mut head = runtime.load(&path).unwrap();
    let neutral = capture(&mut head, 0.0);
    // 使用非正方形视口和偏移布局，对照显式表达/动作路径检查点击产生的像素。
    // Head 与 Body 的框重叠，Body 列在前面也必须优先命中 Head。
    assert_eq!(head.tap(0.63, 0.30).unwrap(), TapHit::Head);
    let mut direct = runtime.load(&path).unwrap();
    direct.play_expression("F04").unwrap();
    let expressed = settle(&mut direct);
    assert!(expressed != neutral);
    assert!(settle(&mut head) == expressed);

    let mut body = runtime.load(&path).unwrap();
    capture(&mut body, 0.0);
    assert_eq!(body.tap(0.63, 0.70).unwrap(), TapHit::Body);
    let mut direct = runtime.load(&path).unwrap();
    direct.play_motion("TapBody", 0).unwrap();
    direct.play_expression("F04").unwrap();
    let animated = settle(&mut direct);
    assert!(animated != expressed);
    assert!(settle(&mut body) == animated);

    let mut miss = runtime.load(&path).unwrap();
    miss.set_look(Some((0.65, -0.45))).unwrap();
    assert!(capture(&mut miss, 0.0) != neutral);
    for (x, y) in [
        (f32::NAN, 0.5),
        (0.5, f32::INFINITY),
        (-0.01, 0.5),
        (0.5, 1.01),
    ] {
        assert!(miss.tap(x, y).is_err());
    }
    assert!(capture(&mut miss, 0.0) != neutral);
    assert_eq!(miss.tap(0.0, 0.42).unwrap(), TapHit::Miss);
    assert!(settle(&mut miss) == neutral, "a valid miss clears look");

    // 只有未声明命中区才按屏幕上下分区。中线归 Body，缺少动作和表情时仍可返回命中结果。
    let fallback = fixture.model(
        "fallback.model3.json",
        "",
        r#", "Layout":{"y":-0.24
        }"#,
    );
    let mut model = runtime.load(&fallback).unwrap();
    assert_eq!(model.tap(0.5, 0.49).unwrap(), TapHit::Head);
    assert_eq!(model.tap(0.5, 0.5).unwrap(), TapHit::Body);
    let custom = fixture.model(
        "custom.model3.json",
        "",
        r#", "HitAreas":[{"Name":"Custom","Id":"HitArea"}]"#,
    );
    let mut model = runtime.load(&custom).unwrap();
    assert_eq!(model.tap(0.5, 0.25).unwrap(), TapHit::Miss);

    // 旧版不允许模型在透明动画期间命中，回退分区也遵守此规则。
    fixture.motion("opacity.motion3.json", "Model", "Opacity", 0.5);
    let faded = fixture.model(
        "faded.model3.json",
        r#", "Motions":{"Fade":[{"File":"opacity.motion3.json"}]}"#,
        "",
    );
    let mut faded = runtime.load(&faded).unwrap();
    assert_eq!(faded.tap(0.5, 0.25).unwrap(), TapHit::Head);
    faded.play_motion("Fade", 0).unwrap();
    settle(&mut faded);
    assert_eq!(faded.tap(0.5, 0.25).unwrap(), TapHit::Miss);
}

#[test]
#[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
fn explicit_device_selection_reports_active_gpu_and_rejects_unknown_devices() {
    use cubism_bridge::{DeviceSelector, Runtime};
    use std::path::Path;

    let _serial = NATIVE_TEST.lock().unwrap();
    // 输入无效或找不到设备时返回错误，随后仍可用 auto 初始化。
    for selector in [
        DeviceSelector::Pci("not-a-pci-address"),
        DeviceSelector::RenderNode(Path::new("renderD128")),
        DeviceSelector::RenderNode(Path::new("/dev/dri/does-not-exist")),
    ] {
        assert!(Runtime::new_on_device(96, 96, selector).is_err());
    }
    let auto = Runtime::new(96, 96).unwrap();
    if let (Some(active), Ok(devices)) = (auto.active_device(), Runtime::devices()) {
        assert!(devices.contains(&active));
    }
    drop(auto);

    // 指定本机 PCI 后，用该地址和 render node 两次初始化，核对活动设备并实际渲染。
    let second = std::env::var("CUBISM_TEST_SECOND_PCI").ok();
    let Ok(pci) = std::env::var("CUBISM_TEST_PCI") else {
        assert!(second.is_none(), "set CUBISM_TEST_PCI for the first GPU");
        return;
    };
    if let Some(second) = &second {
        assert!(
            !pci.eq_ignore_ascii_case(second),
            "select two distinct GPUs"
        );
        assert_ne!(
            std::env::var("LIBGL_ALWAYS_SOFTWARE").as_deref(),
            Ok("1"),
            "disable the software override for the two-GPU test"
        );
    }
    verify_hardware_device(&pci, "CUBISM_TEST_RENDERER");
    if let Some(second) = second {
        // 单进程释放第一块卡的 Cubism/EGL 资源，再在第二块卡上核对实际设备和样例像素。
        verify_hardware_device(&second, "CUBISM_TEST_SECOND_RENDERER");
    }
}

fn verify_hardware_device(pci: &str, renderer_env: &str) {
    use cubism_bridge::{DeviceSelector, Runtime, rgba_len};
    use std::path::PathBuf;

    let pci = pci.to_ascii_lowercase();
    let devices = Runtime::devices().unwrap();
    let expected = devices
        .iter()
        .find(|device| device.pci.as_deref() == Some(pci.as_str()))
        .unwrap_or_else(|| panic!("missing test device {pci} in {devices:?}"));
    let node = expected.render_node.as_deref().unwrap();
    let by_path = PathBuf::from(format!("/dev/dri/by-path/pci-{pci}-render"));
    let model_path = PathBuf::from(std::env::var_os("CUBISM_SDK_DIR").unwrap())
        .join("Samples/Resources/Haru/Haru.model3.json");
    let pci_uppercase = pci.to_ascii_uppercase();
    let mut selectors = vec![
        DeviceSelector::Pci(&pci_uppercase),
        DeviceSelector::RenderNode(node),
    ];
    if by_path.exists() {
        selectors.push(DeviceSelector::RenderNode(&by_path));
    }
    for selector in selectors {
        let runtime = Runtime::new_on_device(96, 96, selector);
        // Mesa 曾在同时强制软件渲染和选硬件设备时崩溃。开启该变量应清楚报错。
        if std::env::var("LIBGL_ALWAYS_SOFTWARE").as_deref() == Ok("1") {
            let Err(error) = runtime else {
                panic!("software override unexpectedly accepted the hardware selector");
            };
            assert!(
                error.to_string().contains("LIBGL_ALWAYS_SOFTWARE"),
                "{error}"
            );
            continue;
        }
        let runtime = runtime.unwrap();
        assert_eq!(runtime.active_device().as_ref(), Some(expected));
        let renderer = runtime.renderer();
        if let Ok(expected_renderer) = std::env::var(renderer_env) {
            assert!(
                renderer.contains(&expected_renderer),
                "unexpected renderer: {renderer}"
            );
        }
        eprintln!("{selector:?}: {expected:?} via {renderer}");
        let mut model = runtime.load(&model_path).unwrap();
        let mut pixels = vec![0; rgba_len(96, 96).unwrap()];
        model.render(0.0, &mut pixels).unwrap();
        assert!(pixels.chunks_exact(4).any(|p| p[3] == 255 && p[0] != p[1]));
    }
}

#[test]
#[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
fn expression_toggles_fade_expire_and_reset_independently() {
    use cubism_bridge::Runtime;
    use std::path::Path;

    let _serial = NATIVE_TEST.lock().unwrap();
    let fixture = HaruFixture::new();
    let a = Path::new("angle-x.exp3.json");
    let b = Path::new("angle-y.exp3.json");
    for (file, content) in [
        (a, r#"{"Parameters":[{"Id":"ParamAngleX","Value":22}]}"#),
        (
            b,
            r#"{"Parameters":[{"Id":"ParamAngleY","Blend":"Overwrite","Value":-18}]}"#,
        ),
    ] {
        std::fs::write(fixture.0.join(file), content).unwrap();
    }
    let path = fixture.model(
        "toggles.model3.json",
        r#", "Expressions":[{"Name":"F04","File":"expressions/F04.exp3.json"}]"#,
        "",
    );
    let runtime = Runtime::new(TEST_WIDTH, TEST_HEIGHT).unwrap();
    let mut model = runtime.load(&path).unwrap();
    let mut reference = runtime.load(&path).unwrap();
    let neutral = capture(&mut model, 0.0);
    // Compact JSON is accepted; toggles affect only the model that plays them.
    assert!(model.toggle_expression(a, 0.5, None).unwrap());
    assert!(!reference.expression_toggle_active(a).unwrap());
    assert!(capture(&mut model, 0.0) == neutral);
    let halfway = capture(&mut model, 0.25);
    let full_a = capture(&mut model, 0.25);
    assert!(halfway != neutral && halfway != full_a);
    assert!(
        capture(&mut model, 1.0) == full_a,
        "additive toggle accumulated"
    );
    assert!(capture(&mut reference, 0.0) == neutral);

    // Timer expiry changes the target immediately, but keeps fading independently.
    assert!(model.toggle_expression(b, 0.25, Some(0.5)).unwrap());
    let both = capture(&mut model, 0.25);
    assert!(both != full_a);
    assert!(capture(&mut model, 0.25) == both);
    assert!(model.expression_toggle_active(a).unwrap());
    assert!(!model.expression_toggle_active(b).unwrap());
    assert!(capture(&mut model, 0.25) == full_a);
    assert!(
        !model
            .toggle_expression(Path::new("./angle-x.exp3.json"), 0.5, None)
            .unwrap()
    );
    assert!(capture(&mut model, 0.25) == halfway);
    assert!(capture(&mut model, 0.25) == neutral);

    // An ordinary expression survives by default. Setting its timeout clears only
    // that queue; resetting the toggle must likewise preserve a new ordinary expression.
    model.play_expression("F04").unwrap();
    settle(&mut model);
    let ordinary = capture(&mut model, 1.0);
    assert!(ordinary != neutral && capture(&mut model, 1.0) == ordinary);
    assert!(model.toggle_expression(a, 0.0, None).unwrap());
    assert!(capture(&mut model, 0.0) != ordinary);
    model.set_expression_timeout(Some(0.5)).unwrap();
    assert!(capture(&mut model, 0.0) == full_a);
    assert!(model.expression_toggle_active(a).unwrap());
    model.set_expression_timeout(None).unwrap();
    model.play_expression("F04").unwrap();
    settle(&mut model);
    capture(&mut model, 1.0);
    model.reset_expression_toggles();
    assert!(!model.expression_toggle_active(a).unwrap());
    assert!(capture(&mut model, 0.0) == ordinary);

    assert!(model.set_expression_timeout(Some(-1.0)).is_err());
    assert!(model.toggle_expression(a, f32::NAN, None).is_err());
    assert!(
        model
            .toggle_expression(a, 0.0, Some(f32::INFINITY))
            .is_err()
    );
    assert!(
        model
            .toggle_expression(Path::new("../outside.exp3.json"), 0.0, None)
            .is_err()
    );
    assert!(
        model
            .toggle_expression(Path::new("missing.exp3.json"), 0.0, None)
            .is_err()
    );
}
