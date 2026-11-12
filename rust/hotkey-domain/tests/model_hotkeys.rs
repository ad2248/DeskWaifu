//! Ports of `plugins/desktop/tests/ModelHotkeysTest.cpp`, plus the parsing edge
//! cases that test only reaches through its fixtures.

use deskwaifu_hotkey_domain::{
    ApplyOp, Blend, FsReader, Hotkeys, Reader, TriggerError, DISABLED, EXPRESSION_MISSING,
    INVALID_CONFIGURATION, MISSING_OR_DUPLICATE_ID, NOT_FOUND, RELEASE_TO_DEACTIVATE,
    UNSUPPORTED_ACTION,
};
use std::collections::HashMap;
use std::path::PathBuf;

/// In-memory model directory.
#[derive(Default)]
struct Memory(HashMap<String, String>);

impl Memory {
    fn with(mut self, path: &str, contents: impl Into<String>) -> Self {
        self.0.insert(path.to_string(), contents.into());
        self
    }

    fn hotkeys(mut self, bindings: &str) -> Self {
        self.0.insert(
            "test.vtube.json".to_string(),
            format!(r#"{{"Hotkeys":[{bindings}]}}"#),
        );
        self
    }
}

impl Reader for Memory {
    fn read(&self, path: &str) -> Option<String> {
        self.0.get(path).cloned()
    }
}

/// One VTube Studio hotkey entry with the same defaults as the C++ fixture.
fn binding(id: &str, file: &str) -> String {
    format!(
        r#"{{"HotkeyID":"{id}","Name":"","Action":"ToggleExpression","File":"{file}","Triggers":{{"Trigger1":"Tab"}},"IsActive":true,"IsGlobal":true,"FadeSecondsAmount":0.5}}"#
    )
}

/// Minimal expression with one `Add` parameter.
fn expression(parameter: &str) -> String {
    format!(r#"{{"Parameters":[{{"Id":"{parameter}","Value":1,"Blend":"Add"}}]}}"#)
}

fn loaded(reader: &Memory) -> Hotkeys {
    let mut hotkeys = Hotkeys::new();
    hotkeys.load(reader, "test.model3.json");
    hotkeys
}

fn weight(hotkeys: &Hotkeys, index: usize) -> f32 {
    hotkeys.expressions()[index].weight
}

#[test]
fn discovers_expressions_and_keeps_independent_persistent_state() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{}",
            binding("watermark", "水印.exp3.json"),
            r#"{"HotkeyID":"blush","Action":"ToggleExpression","File":"脸红.exp3.json","Triggers":{"Trigger1":"N1"},"IsActive":true}"#
        ))
        .with(
            "exp/水印.exp3.json",
            r#"{"Type":"Live2D Expression","Parameters":[{"Id":"ParamAngleX","Value":1,"Blend":"Add"}]}"#,
        )
        .with(
            "exp/脸红.exp3.json",
            r#"{"Parameters":[{"Id":"ParamAngleY","Value":2,"Blend":"Overwrite"}]}"#,
        );
    let mut hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings().len(), 2);
    assert_eq!(hotkeys.bindings()[0].keys, ["Tab"]);
    assert_eq!(hotkeys.bindings()[0].name, "水印.exp3.json");
    assert_eq!(hotkeys.bindings()[0].reason, "");

    hotkeys.trigger("watermark").unwrap();
    hotkeys.update(0.25);
    assert_eq!(weight(&hotkeys, 0), 0.5);
    hotkeys.trigger("blush").unwrap();
    hotkeys.update(10.0);
    assert!(hotkeys.is_active(&hotkeys.bindings()[0].id));
    assert!(hotkeys.is_active(&hotkeys.bindings()[1].id));

    hotkeys.trigger("watermark").unwrap();
    hotkeys.update(0.25);
    assert_eq!(weight(&hotkeys, 0), 0.5);
    hotkeys.update(0.25);
    assert_eq!(weight(&hotkeys, 0), 0.0);
    assert!(hotkeys.is_active(&hotkeys.bindings()[1].id));

    let token = hotkeys.token().to_string();
    hotkeys.reset();
    assert_ne!(token, hotkeys.token());
    assert!(!hotkeys.is_active(&hotkeys.bindings()[1].id));
    assert_eq!(weight(&hotkeys, 1), 0.0);

    hotkeys.load(&reader, "other.model3.json");
    assert_ne!(token, hotkeys.token());
    assert!(hotkeys.bindings().is_empty());
    assert!(hotkeys.expressions().is_empty());
    assert_eq!(hotkeys.trigger("watermark"), Err(TriggerError::NotFound));
}

#[test]
fn resolves_only_current_binding_ids() {
    let reader = Memory::default()
        .hotkeys(&binding("watermark", "水印.exp3.json"))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let mut hotkeys = Hotkeys::new();
    let mut other = Hotkeys::new();
    other.load(&reader, "test.model3.json");
    // Public queries resolve by ID against the current model, never by foreign index.
    assert!(!hotkeys.is_active(&other.bindings()[0].id));

    hotkeys.load(&reader, "test.model3.json");
    let old = hotkeys.bindings()[0].clone();
    let token = hotkeys.token().to_string();
    hotkeys.trigger(&old.id).unwrap();
    assert!(hotkeys.is_active(&old.id));
    assert!(!other.is_active(&old.id));
    assert!(!hotkeys.is_active("missing"));
    assert!(!hotkeys.is_active(""));

    let replaced = Memory::default()
        .hotkeys(&binding("replacement", "脸红.exp3.json"))
        .with("exp/脸红.exp3.json", expression("ParamAngleY"));
    hotkeys.load(&replaced, "test.model3.json");
    assert_ne!(token, hotkeys.token());
    hotkeys.trigger("replacement").unwrap();
    assert!(hotkeys.is_active("replacement"));
    assert!(!hotkeys.is_active(&old.id));

    hotkeys.load(&reader, "absent.model3.json");
    assert!(!hotkeys.is_active("replacement"));
}

#[test]
fn omitted_non_model3_and_malformed_configurations() {
    let empty = Memory::default();
    let mut hotkeys = loaded(&empty);
    assert!(hotkeys.bindings().is_empty());
    assert_eq!(hotkeys.error(), "");

    hotkeys.load(
        &Memory::default().hotkeys(&binding("watermark", "水印.exp3.json")),
        "test.model3",
    );
    assert!(hotkeys.bindings().is_empty());
    assert_eq!(hotkeys.error(), "");

    hotkeys.load(
        &Memory::default().with("test.vtube.json", "{invalid"),
        "test.model3.json",
    );
    assert_eq!(hotkeys.error(), INVALID_CONFIGURATION);
    assert!(hotkeys.bindings().is_empty());
    assert!(hotkeys.expressions().is_empty());

    hotkeys.load(
        &Memory::default().with("test.vtube.json", r#"{"Hotkeys":{}}"#),
        "test.model3.json",
    );
    assert_eq!(hotkeys.error(), INVALID_CONFIGURATION);

    hotkeys.load(
        &Memory::default().with("test.vtube.json", r#"{"Hotkeys":"x"}"#),
        "test.model3.json",
    );
    assert_eq!(hotkeys.error(), INVALID_CONFIGURATION);
}

#[test]
fn invalid_and_duplicate_ids_track_every_seen_id() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{},{},{},{}",
            binding("", "水印.exp3.json"),
            r#"{"HotkeyID":"first","Action":"ChangeBackground","File":"水印.exp3.json"}"#,
            r#"{"HotkeyID":"first","Action":"ToggleExpression","File":"水印.exp3.json"}"#,
            r#"{"HotkeyID":"same","Action":"ToggleExpression","File":"水印.exp3.json"}"#,
            r#"{"HotkeyID":"same","Action":"ToggleExpression","File":"水印.exp3.json"}"#,
        ))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    let reasons: Vec<&str> = hotkeys
        .bindings()
        .iter()
        .map(|binding| binding.reason.as_str())
        .collect();
    assert_eq!(reasons[0], MISSING_OR_DUPLICATE_ID);
    assert_eq!(reasons[1], UNSUPPORTED_ACTION);
    assert_eq!(reasons[2], MISSING_OR_DUPLICATE_ID);
    assert_eq!(reasons[3], "");
    assert_eq!(reasons[4], MISSING_OR_DUPLICATE_ID);
    assert_eq!(hotkeys.bindings()[3].expression, Some(0));
}

#[test]
fn unsupported_action_and_release_to_deactivate_win_over_resolution() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{},{},{}",
            r#"{"HotkeyID":"action","Action":"ChangeBackground","File":"水印.exp3.json"}"#,
            r#"{"HotkeyID":"release","Action":"ToggleExpression","DeactivateAfterKeyUp":true,"File":"水印.exp3.json"}"#,
            r#"{"HotkeyID":"release-false","Action":"ToggleExpression","DeactivateAfterKeyUp":false,"File":"水印.exp3.json"}"#,
            r#"{"HotkeyID":"release-string","Action":"ToggleExpression","DeactivateAfterKeyUp":"true","File":"水印.exp3.json"}"#,
        ))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings()[0].reason, UNSUPPORTED_ACTION);
    assert_eq!(hotkeys.bindings()[0].expression, None);
    assert_eq!(hotkeys.bindings()[1].reason, RELEASE_TO_DEACTIVATE);
    assert_eq!(hotkeys.bindings()[2].reason, "");
    // Only JSON booleans count as truthy, matching CubismJson's ToBoolean.
    assert_eq!(hotkeys.bindings()[3].reason, "");

    // Validation is a chain: the first failing check owns the reason.
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{},{}",
            r#"{"HotkeyID":"both","Action":"ChangeBackground","DeactivateAfterKeyUp":true,"File":"水印.exp3.json"}"#,
            r#"{"HotkeyID":"release-missing","Action":"ToggleExpression","DeactivateAfterKeyUp":true,"File":"absent.exp3.json"}"#,
            r#"{"HotkeyID":"","Action":"ChangeBackground","File":"水印.exp3.json"}"#,
        ))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings()[0].reason, UNSUPPORTED_ACTION);
    assert_eq!(hotkeys.bindings()[1].reason, RELEASE_TO_DEACTIVATE);
    assert_eq!(hotkeys.bindings()[2].reason, MISSING_OR_DUPLICATE_ID);

    // A non-object entry has no fields, so it reports the ID reason.
    let hotkeys = loaded(&Memory::default().hotkeys(r#""not-an-object""#));
    assert_eq!(hotkeys.bindings().len(), 1);
    assert_eq!(hotkeys.bindings()[0].reason, MISSING_OR_DUPLICATE_ID);

    // An empty but present array is a valid configuration with no bindings.
    let hotkeys = loaded(&Memory::default().hotkeys(""));
    assert_eq!(hotkeys.error(), "");
    assert!(hotkeys.bindings().is_empty());
}

#[test]
fn fills_defaults_and_collects_trigger_keys() {
    let reader = Memory::default()
        .hotkeys(
            r#"{"HotkeyID":"defaults","Name":"","Action":"ToggleExpression","File":"水印.exp3.json",
                "Triggers":{"Trigger1":"Tab","Trigger2":"","Trigger3":"N1"},"FadeSecondsAmount":-2}"#,
        )
        .with("水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    let binding = &hotkeys.bindings()[0];
    assert_eq!(binding.name, "水印.exp3.json");
    assert_eq!(binding.file, "水印.exp3.json");
    assert_eq!(binding.keys, ["Tab", "N1"]);
    assert!(binding.enabled);
    assert_eq!(binding.fade, 0.0);
    assert_eq!(binding.duration, -1.0);
    // The model-root candidate resolves the expression directly.
    assert_eq!(binding.expression, Some(0));
    assert_eq!(hotkeys.expressions()[0].file, "水印.exp3.json");

    let reader = Memory::default()
        .hotkeys(
            r#"{"HotkeyID":"named","Name":"Watermark","Action":"ToggleExpression","File":"水印.exp3.json",
                "IsActive":false,"FadeSecondsAmount":1.25,"DeactivateAfterSeconds":true,"DeactivateAfterSecondsAmount":3}"#,
        )
        .with("水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    let binding = &hotkeys.bindings()[0];
    assert_eq!(binding.name, "Watermark");
    assert!(!binding.enabled);
    assert_eq!(binding.fade, 1.25);
    assert_eq!(binding.duration, 3.0);
    assert!(binding.keys.is_empty());

    let reader = Memory::default()
        .hotkeys(
            r#"{"HotkeyID":"fallbacks","Action":"ToggleExpression","File":"水印.exp3.json",
                "FadeSecondsAmount":"fast","DeactivateAfterSeconds":true,"DeactivateAfterSecondsAmount":-1}"#,
        )
        .with("水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings()[0].fade, 0.5);
    assert_eq!(hotkeys.bindings()[0].duration, 0.0);

    let reader = Memory::default()
        .hotkeys(
            r#"{"HotkeyID":"absurd","Action":"ToggleExpression","File":"水印.exp3.json",
                "DeactivateAfterSeconds":true,"DeactivateAfterSecondsAmount":"ten"}"#,
        )
        .with("水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings()[0].duration, 10.0);

    // A value wider than f32 becomes non-finite and falls back like the C++ guard.
    let reader = Memory::default()
        .hotkeys(
            r#"{"HotkeyID":"huge","Action":"ToggleExpression","File":"水印.exp3.json",
                "FadeSecondsAmount":1e39,"DeactivateAfterSeconds":true,"DeactivateAfterSecondsAmount":1e39}"#,
        )
        .with("水印.exp3.json", expression("ParamAngleX"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings()[0].fade, 0.5);
    assert_eq!(hotkeys.bindings()[0].duration, 10.0);
}

#[test]
fn resolves_folder_relative_then_each_expression_directory() {
    let folder = Memory::default()
        .hotkeys(
            r#"{"HotkeyID":"folder","Action":"ToggleExpression","File":"x.exp3.json","Folder":"Custom"}"#,
        )
        .with("Custom/x.exp3.json", expression("ParamA"))
        .with("x.exp3.json", expression("ParamRoot"));
    let hotkeys = loaded(&folder);
    assert_eq!(hotkeys.bindings()[0].expression, Some(0));
    assert_eq!(hotkeys.expressions()[0].file, "Custom/x.exp3.json");

    for (directory, expected) in [
        ("", "x.exp3.json"),
        ("Expressions/", "Expressions/x.exp3.json"),
        ("exp/", "exp/x.exp3.json"),
    ] {
        let reader = Memory::default()
            .hotkeys(&binding("found", "x.exp3.json"))
            .with(&format!("{directory}x.exp3.json"), expression("ParamA"));
        let hotkeys = loaded(&reader);
        assert_eq!(hotkeys.bindings()[0].expression, Some(0));
        assert_eq!(hotkeys.expressions()[0].file, expected);
    }

    // The first candidate that parses wins, even before later directories.
    let reader = Memory::default()
        .hotkeys(&binding("found", "x.exp3.json"))
        .with("Expressions/x.exp3.json", expression("ParamEarly"))
        .with("exp/x.exp3.json", expression("ParamLate"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.expressions().len(), 1);
    assert_eq!(hotkeys.expressions()[0].file, "Expressions/x.exp3.json");
    assert_eq!(hotkeys.expressions()[0].parameters[0].id, "ParamEarly");

    // A broken folder candidate is skipped rather than reported as missing.
    let reader = Memory::default()
        .hotkeys(
            r#"{"HotkeyID":"broken-folder","Action":"ToggleExpression","File":"x.exp3.json","Folder":"Custom"}"#,
        )
        .with("Custom/x.exp3.json", "{invalid")
        .with("exp/x.exp3.json", expression("ParamFallback"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings()[0].reason, "");
    assert_eq!(hotkeys.expressions()[0].file, "exp/x.exp3.json");
    assert_eq!(hotkeys.expressions()[0].parameters[0].id, "ParamFallback");
}

#[test]
fn rejects_absolute_parent_and_backslash_folders() {
    for folder in [
        "/absolute",
        "../escape",
        "nested/../escape",
        "windows\\path",
    ] {
        let folder = serde_json::Value::from(folder).to_string();
        let reader = Memory::default()
            .hotkeys(&format!(
                r#"{{"HotkeyID":"bad","Action":"ToggleExpression","File":"x.exp3.json","Folder":{folder}}}"#
            ))
            .with("x.exp3.json", expression("ParamRoot"));
        let hotkeys = loaded(&reader);
        assert_eq!(hotkeys.error(), "", "{folder}");
        assert_eq!(hotkeys.bindings()[0].expression, Some(0), "{folder}");
        assert_eq!(hotkeys.expressions()[0].file, "x.exp3.json", "{folder}");
    }

    // A file name that escapes the model directory is rejected at every prefix.
    let reader = Memory::default()
        .hotkeys(&binding("escape", "../outside.exp3.json"))
        .with("../outside.exp3.json", expression("ParamA"));
    let hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings()[0].reason, EXPRESSION_MISSING);
    assert!(hotkeys.expressions().is_empty());
}

#[test]
fn rejects_missing_and_invalid_expression_files() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{},{}",
            binding("missing", "absent.exp3.json"),
            binding("garbage", "garbage.exp3.json"),
            binding("no-parameters", "bare.exp3.json"),
        ))
        .with("exp/garbage.exp3.json", "{invalid")
        .with("exp/bare.exp3.json", r#"{"Parameters":{}}"#);
    let hotkeys = loaded(&reader);
    for binding in hotkeys.bindings() {
        assert_eq!(binding.reason, EXPRESSION_MISSING);
        assert_eq!(binding.expression, None);
    }
    assert!(hotkeys.expressions().is_empty());
}

#[test]
fn an_empty_parameter_list_blocks_fallback_and_dedupes_by_path() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{},{}",
            binding("empty", "empty.exp3.json"),
            binding("first", "shared.exp3.json"),
            binding("second", "shared.exp3.json"),
        ))
        .with("Expressions/empty.exp3.json", r#"{"Parameters":[]}"#)
        .with("exp/empty.exp3.json", expression("ParamLater"))
        .with("exp/shared.exp3.json", expression("ParamShared"));
    let hotkeys = loaded(&reader);
    // The empty candidate still stops the fallback and keeps its table entry.
    assert_eq!(hotkeys.bindings()[0].reason, EXPRESSION_MISSING);
    assert_eq!(hotkeys.bindings()[0].expression, None);
    assert_eq!(hotkeys.bindings()[1].expression, Some(1));
    assert_eq!(hotkeys.bindings()[2].expression, Some(1));
    assert_eq!(hotkeys.expressions().len(), 2);
    assert_eq!(hotkeys.expressions()[0].file, "Expressions/empty.exp3.json");
    assert!(hotkeys.expressions()[0].parameters.is_empty());
    assert_eq!(hotkeys.expressions()[1].file, "exp/shared.exp3.json");
}

#[test]
fn parameters_default_blend_and_skip_invalid_entries() {
    let reader = Memory::default()
        .hotkeys(&binding("mixed", "mixed.exp3.json"))
        .with(
            "exp/mixed.exp3.json",
            r#"{"Parameters":[
                {"Id":"Defaulted","Value":1},
                {"Id":"Blank","Value":2,"Blend":""},
                {"Id":"Multiply","Value":3,"Blend":"Multiply"},
                {"Id":"Overwrite","Value":4,"Blend":"Overwrite"},
                {"Id":"","Value":5,"Blend":"Add"},
                {"Id":"StringValue","Value":"6","Blend":"Add"},
                {"Id":"MissingValue","Blend":"Add"},
                {"Id":"BadBlend","Value":7,"Blend":"Replace"},
                {"Id":"BooleanValue","Value":true,"Blend":"Add"},
                {"Id":"Fraction","Value":0.25,"Blend":"Add"}
            ]}"#,
        );
    let hotkeys = loaded(&reader);
    let parameters = &hotkeys.expressions()[0].parameters;
    assert_eq!(parameters.len(), 5);
    assert_eq!(parameters[0].id, "Defaulted");
    assert_eq!(parameters[0].blend, Blend::Add);
    assert_eq!(parameters[1].id, "Blank");
    assert_eq!(parameters[1].blend, Blend::Add);
    assert_eq!(parameters[2].blend, Blend::Multiply);
    assert_eq!(parameters[3].blend, Blend::Overwrite);
    assert_eq!(parameters[4].id, "Fraction");
    assert_eq!(parameters[4].value, 0.25);
}

#[test]
fn toggles_activation_and_reports_unavailable_bindings() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{},{},{}",
            binding("ok", "水印.exp3.json"),
            r#"{"HotkeyID":"disabled","Action":"ToggleExpression","File":"水印.exp3.json","IsActive":false}"#,
            r#"{"HotkeyID":"unsupported","Action":"ChangeBackground","File":"水印.exp3.json"}"#,
            binding("missing", "absent.exp3.json"),
        ))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let mut hotkeys = loaded(&reader);
    assert_eq!(hotkeys.bindings().len(), 4);

    assert_eq!(hotkeys.trigger("disabled"), Err(TriggerError::Disabled));
    assert_eq!(
        hotkeys.trigger("unsupported"),
        Err(TriggerError::Invalid(UNSUPPORTED_ACTION.to_string()))
    );
    assert_eq!(
        hotkeys.trigger("missing"),
        Err(TriggerError::Invalid(EXPRESSION_MISSING.to_string()))
    );
    assert_eq!(hotkeys.trigger("unknown"), Err(TriggerError::NotFound));
    assert!(hotkeys.trigger("ok").is_ok());
    assert!(hotkeys.is_active("ok"));
    assert!(hotkeys.trigger("ok").is_ok());
    assert!(!hotkeys.is_active("ok"));

    assert_eq!(TriggerError::NotFound.reason(), NOT_FOUND);
    assert_eq!(TriggerError::Disabled.reason(), DISABLED);
    assert_eq!(TriggerError::NotFound.to_string(), NOT_FOUND);
    assert_eq!(hotkeys.error(), "");
}

#[test]
fn tokens_increase_across_resets_and_loads_without_repeating() {
    let reader = Memory::default()
        .hotkeys(&binding("watermark", "水印.exp3.json"))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let mut hotkeys = Hotkeys::new();
    assert_eq!(hotkeys.token(), "");
    let first = {
        hotkeys.load(&reader, "test.model3.json");
        hotkeys.token().to_string()
    };
    let mut seen = std::collections::HashSet::from([first]);
    for _ in 0..3 {
        hotkeys.load(&reader, "test.model3.json");
        assert!(seen.insert(hotkeys.token().to_string()));
    }
    hotkeys.reset();
    assert!(seen.insert(hotkeys.token().to_string()));
    let mut other = Hotkeys::new();
    other.load(&reader, "test.model3.json");
    assert!(seen.insert(other.token().to_string()));
    assert!(seen
        .iter()
        .all(|token| token.starts_with("model-") && token.len() > "model-".len()));
}

#[test]
fn fades_toward_target_weight_with_and_without_fade_duration() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{}",
            binding("faded", "水印.exp3.json"),
            r#"{"HotkeyID":"instant","Action":"ToggleExpression","File":"水印.exp3.json","FadeSecondsAmount":0}"#,
        ))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let mut hotkeys = loaded(&reader);
    hotkeys.trigger("faded").unwrap();
    hotkeys.update(0.1);
    assert!((weight(&hotkeys, 0) - 0.2).abs() < 1e-6);
    hotkeys.update(0.1);
    assert!((weight(&hotkeys, 0) - 0.4).abs() < 1e-6);
    hotkeys.trigger("faded").unwrap();
    hotkeys.update(0.1);
    assert!((weight(&hotkeys, 0) - 0.2).abs() < 1e-6);

    // A zero fade reaches the target in one update and clamps at both ends.
    hotkeys.trigger("instant").unwrap();
    hotkeys.update(0.001);
    assert_eq!(weight(&hotkeys, 0), 1.0);
    hotkeys.update(5.0);
    assert_eq!(weight(&hotkeys, 0), 1.0);
    hotkeys.trigger("instant").unwrap();
    hotkeys.update(5.0);
    assert_eq!(weight(&hotkeys, 0), 0.0);
    hotkeys.update(5.0);
    assert_eq!(weight(&hotkeys, 0), 0.0);
}

#[test]
fn deactivates_after_the_configured_duration() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{}",
            r#"{"HotkeyID":"timed","Action":"ToggleExpression","File":"水印.exp3.json",
                "DeactivateAfterSeconds":true,"DeactivateAfterSecondsAmount":1}"#,
            r#"{"HotkeyID":"instant-timeout","Action":"ToggleExpression","File":"水印.exp3.json",
                "DeactivateAfterSeconds":true,"DeactivateAfterSecondsAmount":0}"#,
        ))
        .with("exp/水印.exp3.json", expression("ParamAngleX"));
    let mut hotkeys = loaded(&reader);
    hotkeys.trigger("timed").unwrap();
    assert_eq!(hotkeys.expressions()[0].remaining, 1.0);
    hotkeys.update(0.5);
    assert!(hotkeys.is_active("timed"));
    hotkeys.update(1.0);
    assert!(!hotkeys.is_active("timed"));
    assert_eq!(weight(&hotkeys, 0), 0.0);
    assert_eq!(hotkeys.expressions()[0].remaining, -0.5);

    // Zero duration deactivates on the first update, before any weight ramp.
    hotkeys.trigger("instant-timeout").unwrap();
    hotkeys.update(0.001);
    assert!(!hotkeys.is_active("instant-timeout"));
    // Deactivating manually restores the no-timeout remaining.
    hotkeys.trigger("timed").unwrap();
    hotkeys.trigger("timed").unwrap();
    assert_eq!(hotkeys.expressions()[0].remaining, -1.0);
}

#[test]
fn apply_lists_ordered_weighted_operations_and_restores_base() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{}",
            binding("watermark", "水印.exp3.json"),
            binding("blush", "脸红.exp3.json"),
        ))
        .with(
            "exp/水印.exp3.json",
            r#"{"Parameters":[{"Id":"ParamAngleX","Value":1,"Blend":"Add"}]}"#,
        )
        .with(
            "exp/脸红.exp3.json",
            r#"{"Parameters":[{"Id":"ParamAngleY","Value":2,"Blend":"Overwrite"}]}"#,
        );
    let mut hotkeys = loaded(&reader);
    assert!(hotkeys.apply().is_empty());

    hotkeys.trigger("watermark").unwrap();
    hotkeys.trigger("blush").unwrap();
    // Simulate the renderer's per-frame pass: restore saved base, then blend in order.
    let saved = HashMap::from([("ParamAngleX", 3.0_f32), ("ParamAngleY", 4.0_f32)]);
    let mut frame = saved.clone();
    for _ in 0..600 {
        frame = saved.clone();
        hotkeys.update(1.0 / 60.0);
        for operation in hotkeys.apply() {
            blend(&mut frame, &operation);
        }
    }
    assert!((frame["ParamAngleX"] - 4.0).abs() < 1e-4);
    assert!((frame["ParamAngleY"] - 2.0).abs() < 1e-4);

    let operations = hotkeys.apply();
    assert_eq!(operations.len(), 2);
    assert_eq!(operations[0].parameter_id, "ParamAngleX");
    assert_eq!(operations[0].blend, Blend::Add);
    assert_eq!(operations[0].value, 1.0);
    assert_eq!(operations[0].weight, 1.0);
    assert_eq!(operations[1].parameter_id, "ParamAngleY");
    assert_eq!(operations[1].blend, Blend::Overwrite);
    assert_eq!(operations[1].weight, 1.0);

    hotkeys.trigger("watermark").unwrap();
    hotkeys.update(1.0);
    let operations = hotkeys.apply();
    assert_eq!(operations.len(), 1);
    assert_eq!(operations[0].parameter_id, "ParamAngleY");
    assert_eq!(operations[0].weight, 1.0);
}

/// Applies one operation the way `CubismModel` does, using the reported weight.
fn blend(frame: &mut HashMap<&'static str, f32>, operation: &ApplyOp) {
    let current = frame
        .get_mut(operation.parameter_id.as_str())
        .expect("parameter present in the test model");
    *current = match operation.blend {
        Blend::Add => *current + operation.value * operation.weight,
        Blend::Multiply => *current * (1.0 + (operation.value - 1.0) * operation.weight),
        Blend::Overwrite => {
            *current * (1.0 - operation.weight) + operation.value * operation.weight
        }
    };
}

#[test]
fn apply_filters_zero_weight_expressions_in_expression_order() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{}",
            binding("slow", "slow.exp3.json"),
            r#"{"HotkeyID":"fast","Action":"ToggleExpression","File":"fast.exp3.json","FadeSecondsAmount":0}"#,
        ))
        .with(
            "exp/slow.exp3.json",
            r#"{"Parameters":[{"Id":"A","Value":1,"Blend":"Add"},{"Id":"B","Value":2,"Blend":"Add"}]}"#,
        )
        .with("exp/fast.exp3.json", expression("C"));
    let mut hotkeys = loaded(&reader);
    hotkeys.trigger("fast").unwrap();
    hotkeys.update(0.01);
    let operations = hotkeys.apply();
    assert_eq!(operations.len(), 1);
    assert_eq!(operations[0].parameter_id, "C");
    assert_eq!(operations[0].weight, 1.0);

    // Activating the slow expression emits nothing until it has any weight.
    hotkeys.trigger("slow").unwrap();
    assert_eq!(hotkeys.expressions()[0].weight, 0.0);
    assert_eq!(hotkeys.apply().len(), 1);
    hotkeys.update(0.1);
    let operations = hotkeys.apply();
    assert_eq!(operations.len(), 3);
    assert_eq!(operations[0].parameter_id, "A");
    assert_eq!(operations[1].parameter_id, "B");
    assert_eq!(operations[2].parameter_id, "C");
    assert!(operations[..2]
        .iter()
        .all(|op| op.weight == hotkeys.expressions()[0].weight));
}

#[test]
fn updates_return_to_a_quiescent_state() {
    let reader = Memory::default()
        .hotkeys(&format!(
            "{},{}",
            binding("persistent", "水印.exp3.json"),
            r#"{"HotkeyID":"timed","Action":"ToggleExpression","File":"脸红.exp3.json",
                "FadeSecondsAmount":0.5,"DeactivateAfterSeconds":true,"DeactivateAfterSecondsAmount":2}"#,
        ))
        .with("exp/水印.exp3.json", expression("ParamAngleX"))
        .with("exp/脸红.exp3.json", expression("ParamAngleY"));
    let mut hotkeys = loaded(&reader);
    hotkeys.trigger("persistent").unwrap();
    hotkeys.trigger("timed").unwrap();
    for _ in 0..20 {
        hotkeys.update(0.05);
    }
    assert!(hotkeys.is_active("persistent"));
    assert!(hotkeys.is_active("timed"));
    for _ in 0..40 {
        hotkeys.update(0.05);
    }
    assert!(hotkeys.is_active("persistent"));
    assert!(!hotkeys.is_active("timed"));
    // The faded expression keeps ramping down to zero and stays quiet.
    let mut quiet = 0;
    for _ in 0..20 {
        hotkeys.update(0.05);
        if hotkeys.expressions()[1].weight == 0.0 {
            quiet += 1;
        }
    }
    assert_eq!(quiet, 20);
    assert_eq!(hotkeys.expressions()[1].weight, 0.0);
    assert_eq!(hotkeys.expressions()[0].weight, 1.0);
    // The C++ implementation leaves the negative remainder in place after timeout.
    assert!(hotkeys.expressions()[1].remaining < 0.0);
}

#[test]
fn reads_real_files_through_the_filesystem_reader() {
    let base = unique_temp_directory();
    let model = base.join("Model");
    std::fs::create_dir_all(model.join("exp")).unwrap();
    std::fs::write(
        model.join("Model.vtube.json"),
        format!(
            r#"{{"Hotkeys":[{}]}}"#,
            binding("watermark", "水印.exp3.json")
        ),
    )
    .unwrap();
    std::fs::write(model.join("exp/水印.exp3.json"), expression("ParamAngleX")).unwrap();

    let mut hotkeys = Hotkeys::new();
    hotkeys.load(&FsReader::new(&model), "Model.model3.json");
    assert_eq!(hotkeys.error(), "");
    assert_eq!(hotkeys.bindings().len(), 1);
    assert_eq!(hotkeys.expressions()[0].file, "exp/水印.exp3.json");
    hotkeys.trigger("watermark").unwrap();
    assert!(hotkeys.is_active("watermark"));

    std::fs::remove_file(model.join("Model.vtube.json")).unwrap();
    hotkeys.load(&FsReader::new(&model), "Model.model3.json");
    assert_eq!(hotkeys.error(), "");
    assert!(hotkeys.bindings().is_empty());
    std::fs::remove_dir_all(&base).unwrap();
}

fn unique_temp_directory() -> PathBuf {
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let base = std::env::temp_dir().join(format!(
        "deskwaifu-hotkey-domain-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&base).unwrap();
    base
}
