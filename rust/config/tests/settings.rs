use deskwaifu_config::settings::{ConfigDocument, ModelRegion, Settings};

fn decode(text: &str, settings: &mut Settings) -> Result<(), String> {
    Settings::decode(&ConfigDocument::parse(text, true)?, settings)
}
fn round_trip(settings: &Settings) {
    let mut restored = Settings::default();
    decode(&settings.encode().unwrap(), &mut restored).unwrap();
    assert_eq!(&restored, settings);
}
const CONFIGURED: &str = r#"{"additional_model_dirs":["/models"],"default_model":"sample",
    "emotion_timeout":9,"model_scale":2,"model_x":3,"model_y":4,
    "window_width":800,"window_height":600,"hover_opacity_enabled":true,"hover_opacity":0.25,
    "idle_opacity":0.375,"fullscreen_opacity_enabled":true,"fullscreen_opacity":0.25,
    "mouse_look_radius":0.3,"model_region":{"x":1,"y":2,"width":300,"height":400}}"#;

#[test]
fn exact_defaults_and_configured_round_trip() {
    let defaults = Settings::default();
    assert!(defaults.additional_model_dirs.is_empty());
    assert_eq!(
        (
            &defaults.default_model[..],
            &defaults.model_output[..],
            &defaults.render_device[..]
        ),
        ("", "", "")
    );
    assert_eq!(defaults.plugin_dir, "/usr/lib/deskwaifu/plugins");
    assert_eq!(
        (
            defaults.emotion_timeout,
            defaults.model_scale,
            defaults.model_x,
            defaults.model_y
        ),
        (5.0, 1.0, 0.0, 0.0)
    );
    assert_eq!(
        (defaults.window_width, defaults.window_height),
        (1900, 1000)
    );
    assert!(!defaults.hover_opacity_enabled && !defaults.fullscreen_opacity_enabled);
    assert_eq!(
        (
            defaults.hover_opacity,
            defaults.idle_opacity,
            defaults.fullscreen_opacity,
            defaults.mouse_look_radius
        ),
        (0.5, 1.0, 0.5, 0.5)
    );
    assert!(defaults.model_region.is_none() && defaults.unknown_fields.is_empty());
    round_trip(&defaults);
    let document = ConfigDocument::parse(&defaults.encode().unwrap(), false).unwrap();
    assert_eq!(document.values.len(), 15);
    assert!(!document.values.contains_key("sourcePath"));
    let mut settings = defaults.clone();
    decode(CONFIGURED, &mut settings).unwrap();
    assert_eq!(settings.additional_model_dirs, ["/models"]);
    assert_eq!(settings.default_model, "sample");
    assert_eq!(
        (
            settings.emotion_timeout,
            settings.model_scale,
            settings.model_x,
            settings.model_y
        ),
        (9.0, 2.0, 3.0, 4.0)
    );
    assert_eq!((settings.window_width, settings.window_height), (800, 600));
    assert!(settings.hover_opacity_enabled && settings.fullscreen_opacity_enabled);
    assert_eq!(
        (
            settings.hover_opacity,
            settings.idle_opacity,
            settings.fullscreen_opacity,
            settings.mouse_look_radius
        ),
        (0.25, 0.375, 0.25, 0.3)
    );
    assert_eq!(
        settings.model_region,
        Some(ModelRegion {
            x: 1,
            y: 2,
            width: 300,
            height: 400
        })
    );
    assert_eq!(
        ConfigDocument::parse(&settings.encode().unwrap(), false)
            .unwrap()
            .values
            .len(),
        16
    );
    round_trip(&settings);
    decode("{}", &mut settings).unwrap();
    assert_eq!(settings, defaults);
}

#[test]
fn legacy_numeric_conversion_uses_fresh_defaults() {
    let mut settings = Settings::default();
    for value in ["null", "false", "[]", "{}", "\"2\"", "1e1000"] {
        decode(CONFIGURED, &mut settings).unwrap();
        decode(&format!("{{\"emotion_timeout\":{value},\"model_scale\":{value},\"model_x\":{value},\"model_y\":{value},\"window_width\":{value},\"window_height\":{value}}}"), &mut settings).unwrap();
        assert_eq!(settings, Settings::default());
    }
    decode(
        r#"{"model_scale":1e39,"window_width":800.0,"window_height":8e2}"#,
        &mut settings,
    )
    .unwrap();
    assert_eq!(settings, Settings::default());
    // Legacy numbers deliberately have no positivity/range policy beyond their types.
    decode(
        r#"{"model_scale":-2,"emotion_timeout":-1,"window_width":0,"window_height":-1}"#,
        &mut settings,
    )
    .unwrap();
    assert_eq!(
        (
            settings.model_scale,
            settings.emotion_timeout,
            settings.window_width,
            settings.window_height
        ),
        (-2.0, -1.0, 0, -1)
    );
    round_trip(&settings);
}

#[test]
fn typed_fields_and_invalid_decode_preserve_previous_settings() {
    let mut settings = Settings::default();
    decode(CONFIGURED, &mut settings).unwrap();
    let previous = settings.clone();
    for (key, values) in [
        (
            "additional_model_dirs",
            vec!["null", "42", "\"path\"", "[\"path\",1]"],
        ),
        ("default_model", vec!["null", "true", "[]"]),
        ("model_output", vec!["42", "false"]),
        (
            "plugin_dir",
            vec![
                "null",
                "42",
                "true",
                "[]",
                "{}",
                r#""/trusted\u0000/unexpected""#,
            ],
        ),
        ("render_device", vec!["42", "false", r#""intel""#]),
        ("hover_opacity_enabled", vec!["1", "null", r#""true""#]),
        ("fullscreen_opacity_enabled", vec!["0", "[]"]),
    ] {
        for value in values {
            assert!(
                decode(&format!("{{\"{key}\":{value}}}"), &mut settings).is_err(),
                "{key}: {value}"
            );
            assert_eq!(settings, previous);
        }
    }
    assert!(decode(
        r#"{"plugin_dir":"first","plugin_dir":"second"}"#,
        &mut settings
    )
    .is_err());
    assert_eq!(settings, previous);
}

#[test]
fn opacity_radius_region_edges() {
    let mut settings = Settings::default();
    for key in ["hover_opacity", "idle_opacity", "fullscreen_opacity"] {
        for raw in ["-0.01", "1.0001", "null", "\"0.5\"", "1e1000"] {
            assert!(decode(&format!("{{\"{key}\":{raw}}}"), &mut settings).is_err());
        }
        for raw in ["0", "1", "-0", "1e-1000"] {
            decode(&format!("{{\"{key}\":{raw}}}"), &mut settings).unwrap();
        }
    }
    for raw in ["-1", "1e1000", "3.5e38", "null", "\"0\""] {
        assert!(decode(&format!("{{\"mouse_look_radius\":{raw}}}"), &mut settings).is_err());
    }
    for raw in ["0", "-0", "100", "3.4028234663852886e38"] {
        decode(&format!("{{\"mouse_look_radius\":{raw}}}"), &mut settings).unwrap();
        round_trip(&settings);
    }
    for raw in [
        "null",
        "[]",
        "{}",
        r#"{"width":1}"#,
        r#"{"width":0,"height":1}"#,
        r#"{"width":1,"height":-1}"#,
        r#"{"width":1.0,"height":1}"#,
        r#"{"width":1,"height":1,"x":null}"#,
        r#"{"width":1,"height":1,"x":2147483648}"#,
        r#"{"width":1,"height":1,"other":0}"#,
    ] {
        assert!(
            decode(&format!("{{\"model_region\":{raw}}}"), &mut settings).is_err(),
            "{raw}"
        );
    }
    decode(r#"{"model_region":{"width":1,"height":2}}"#, &mut settings).unwrap();
    assert_eq!(
        settings.model_region,
        Some(ModelRegion {
            x: 0,
            y: 0,
            width: 1,
            height: 2
        })
    );
    round_trip(&settings);
}

#[test]
fn render_device_and_plugin_directory_rules() {
    let mut settings = Settings::default();
    for device in ["", "auto", "0000:00:02.0", "abcd:ff:1f.7"] {
        decode(
            &format!("{{\"render_device\":\"{device}\"}}"),
            &mut settings,
        )
        .unwrap();
        assert_eq!(settings.render_device, device);
        round_trip(&settings);
    }
    for device in [
        "AUTO",
        "ABCD:ff:1f.7",
        "0000:00:20.0",
        "0000:00:00.8",
        "0000-00:00.0",
        "0000:00:00.00",
    ] {
        assert!(decode(
            &format!("{{\"render_device\":\"{device}\"}}"),
            &mut settings
        )
        .is_err());
    }
    for directory in ["", "~/models", "/opt/插件 \"quoted\"", "relative\npath"] {
        let raw = serde_json::to_string(directory).unwrap();
        decode(&format!("{{\"plugin_dir\":{raw}}}"), &mut settings).unwrap();
        assert_eq!(settings.plugin_dir, directory);
        round_trip(&settings);
    }
}

#[test]
fn unicode_escape_precision_and_unknown_fields_are_preserved() {
    let mut settings = Settings::default();
    decode(
        r#"{"default_model":"\u5915\u989c\ud83d\ude00","additional_model_dirs":["~\/models"]}"#,
        &mut settings,
    )
    .unwrap();
    assert_eq!(settings.default_model, "夕颜😀");
    assert_eq!(settings.additional_model_dirs, ["~/models"]);
    settings.default_model = "模型 \"quoted\" \\ line\n\t\r\0".into();
    settings.additional_model_dirs = vec![
        "~/models".into(),
        "with \\ and \" and\nnewline".into(),
        "夕颜".into(),
    ];
    settings.model_x = 0.123_456_79;
    settings.model_y = -0.987_654_3;
    settings.model_scale = 1.234_567_9;
    round_trip(&settings);
    for raw in [
        r#""legacy\nkey""#,
        "42",
        "null",
        r#"{"opaque":true}"#,
        "1e1000",
        r#"{"z":1e+02,"a":"\u5915"}"#,
    ] {
        decode(
            &format!("{{\"minimax_api_key\":{raw},\"extension\":{{ \"enabled\":true }} }}"),
            &mut settings,
        )
        .unwrap();
        assert_eq!(settings.unknown_fields["minimax_api_key"], raw);
        assert_eq!(settings.unknown_fields["extension"], r#"{"enabled":true}"#);
        round_trip(&settings);
    }
    settings
        .unknown_fields
        .insert("invalid".into(), "not JSON".into());
    assert!(settings.encode().is_err());
}

#[test]
fn serialization_revalidates_values_and_unknown_collisions() {
    let mut settings = Settings::default();
    for invalid in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
        settings.model_scale = invalid;
        assert!(settings.encode().is_err());
    }
    settings = Settings::default();
    settings.hover_opacity = 2.0;
    assert!(settings.encode().is_err());
    settings = Settings::default();
    settings.mouse_look_radius = -1.0;
    assert!(settings.encode().is_err());
    settings = Settings::default();
    settings
        .unknown_fields
        .insert("default_model".into(), "\"collision\"".into());
    assert!(settings.encode().is_err());
    settings.unknown_fields.clear();
    settings.render_device = "intel".into();
    assert!(settings.encode().is_err());
    settings.render_device.clear();
    settings.plugin_dir = "bad\0path".into();
    assert!(settings.encode().is_err());
    settings.plugin_dir.clear();
    settings.model_region = Some(ModelRegion {
        x: 0,
        y: 0,
        width: 0,
        height: 1,
    });
    assert!(settings.encode().is_err());
    settings.model_region = None;
    settings
        .unknown_fields
        .insert("nested".into(), r#"{"a":1,"\u0061":2}"#.into());
    assert!(settings.encode().is_err());
}

#[test]
fn document_comments_syntax_duplicates_unicode_and_depth() {
    let text = "{ // note\n \"x\":\"夕颜😀 // literal\",\"nested\": [1, // inner\n2]}";
    let doc = ConfigDocument::parse(text, true).unwrap();
    assert_eq!(doc.document, text);
    assert_eq!(doc.values["x"], "\"夕颜😀 // literal\"");
    assert_eq!(doc.values["nested"], "[1,2]");
    assert!(ConfigDocument::parse(text, false).is_err());
    assert!(ConfigDocument::parse(r#"{"url":"https://example//x"}"#, false).is_ok());
    for text in [
        "",
        "[]",
        "null",
        "{",
        r#"{"a":1,}"#,
        r#"{"a":01}"#,
        r#"{"a":1,"a":2}"#,
        r#"{"a":1,"\u0061":2}"#,
        r#"{"nested":[{"é":1,"\u00e9":2}]}"#,
        r#"{"a":NaN}"#,
        r#"{"a":true} trailing"#,
        r#"{"a":"\ud800"}"#,
        r#"{"a":"\udc00"}"#,
        r#"{"a":1e}"#,
        r#"{"a":1.}"#,
        r#"{"a":[1,]}"#,
        r#"{/* block */}"#,
    ] {
        assert!(ConfigDocument::parse(text, true).is_err(), "{text}");
    }
    assert!(ConfigDocument::parse(
        &format!("{{\"a\":{}0{}}}", "[".repeat(64), "]".repeat(64)),
        true
    )
    .is_err());
    assert!(
        ConfigDocument::parse(r#"{"number":1e1000,"escaped":"\\\"//not comment"}"#, true).is_ok()
    );
}
