use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::time::Duration;

const MAX_CONFIG_BYTES: u64 = 256 * 1024;
const MAX_EXPRESSION_BYTES: u64 = 128 * 1024;
const MAX_SHORTCUTS: usize = 128;
const MAX_TEXT_BYTES: usize = 256;
const MAX_PATH_BYTES: usize = 512;
const MAX_TRIGGER_BYTES: usize = 64;
const MAX_SECONDS: f64 = 24.0 * 60.0 * 60.0;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Shortcut {
    pub(crate) id: String,
    pub(crate) name: String,
    pub(crate) keys: Vec<String>,
    pub(crate) action: String,
    pub(crate) enabled: bool,
    pub(crate) expression: Option<PathBuf>,
    pub(crate) fade: Duration,
    pub(crate) duration: Option<Duration>,
    pub(crate) unsupported_reason: Option<String>,
}

/// Loads the exact sidecar for the selected model, without scanning directories.
/// Expression paths are relative to the model directory. Key mapping belongs to the panel.
pub(crate) fn load(model_json: &Path) -> Result<Vec<Shortcut>, String> {
    let Some(stem) = model_json
        .file_name()
        .and_then(|name| name.to_str())
        .and_then(|name| name.strip_suffix(".model3.json"))
        .filter(|stem| !stem.is_empty())
    else {
        return Ok(Vec::new());
    };
    let sidecar = model_json.with_file_name(format!("{stem}.vtube.json"));
    let error = |detail: &dyn std::fmt::Display| {
        format!("VTube Studio hotkeys ({}): {detail}", sidecar.display())
    };
    let path = match sidecar.canonicalize() {
        Ok(path) => path,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(err) => return Err(error(&err)),
    };
    let model_dir = sidecar.parent().filter(|path| !path.as_os_str().is_empty());
    let root = model_dir
        .unwrap_or_else(|| Path::new("."))
        .canonicalize()
        .map_err(|err| error(&err))?;
    if !path.starts_with(&root) {
        return Err(error(&"configuration must stay within the model directory"));
    }
    let config = read_json(&path, MAX_CONFIG_BYTES).map_err(|err| error(&err))?;
    let rows = config["Hotkeys"]
        .as_array()
        .ok_or_else(|| error(&"missing Hotkeys array"))?;
    if rows.len() > MAX_SHORTCUTS {
        return Err(error(&format_args!(
            "at most {MAX_SHORTCUTS} hotkeys are allowed"
        )));
    }

    let mut ids = HashSet::with_capacity(rows.len());
    let mut expressions = HashMap::new();
    let mut shortcuts = Vec::with_capacity(rows.len());
    for row in rows {
        let id = text(&row["HotkeyID"], MAX_TEXT_BYTES).unwrap_or_default();
        let action = text(&row["Action"], MAX_TEXT_BYTES).unwrap_or_default();
        let file = &row["File"];
        let keys = ["Trigger1", "Trigger2", "Trigger3"]
            .into_iter()
            .filter_map(|key| match &row["Triggers"][key] {
                Value::Null => None,
                Value::String(value) if value.is_empty() => None,
                value => Some(
                    text(value, MAX_TRIGGER_BYTES)
                        .unwrap_or("<invalid trigger>")
                        .into(),
                ),
            })
            .collect();
        let mut shortcut = Shortcut {
            id: id.into(),
            name: text(&row["Name"], MAX_TEXT_BYTES)
                .or_else(|| text(file, MAX_TEXT_BYTES))
                .unwrap_or("Unnamed hotkey")
                .into(),
            keys,
            action: action.into(),
            enabled: row["IsActive"].as_bool().unwrap_or(true),
            expression: None,
            fade: seconds(&row["FadeSecondsAmount"], 0.5),
            duration: row["DeactivateAfterSeconds"]
                .as_bool()
                .unwrap_or(false)
                .then(|| seconds(&row["DeactivateAfterSecondsAmount"], 10.0)),
            unsupported_reason: None,
        };
        let reason = if id.is_empty() || !ids.insert(id) {
            Some("Missing, invalid or duplicate hotkey ID")
        } else if action != "ToggleExpression" {
            Some("Unsupported action")
        } else if row["DeactivateAfterKeyUp"].as_bool().unwrap_or(false) {
            Some("Release-to-deactivate is not supported")
        } else {
            shortcut.expression = resolve_expression(&root, &row["Folder"], file, &mut expressions);
            shortcut
                .expression
                .is_none()
                .then_some("Expression file missing or invalid")
        };
        shortcut.unsupported_reason = reason.map(str::to_owned);
        shortcuts.push(shortcut);
    }
    Ok(shortcuts)
}

fn text(value: &Value, max_bytes: usize) -> Option<&str> {
    value.as_str().filter(|text| {
        text.len() <= max_bytes && !text.trim().is_empty() && !text.chars().any(char::is_control)
    })
}

fn seconds(value: &Value, fallback: f64) -> Duration {
    let value = value
        .as_f64()
        .filter(|value| value.is_finite())
        .unwrap_or(fallback);
    Duration::from_secs_f64(value.clamp(0.0, MAX_SECONDS))
}

fn relative_path(value: &Value) -> Option<&Path> {
    let value = text(value, MAX_PATH_BYTES)?;
    if value.contains(['\\', ':'])
        || value
            .split('/')
            .any(|component| matches!(component, "" | "." | ".."))
    {
        return None;
    }
    Some(Path::new(value))
}

fn resolve_expression(
    root: &Path,
    folder: &Value,
    file: &Value,
    cache: &mut HashMap<PathBuf, bool>,
) -> Option<PathBuf> {
    let file = relative_path(file)?;
    if !file.file_name()?.to_str()?.ends_with(".exp3.json") {
        return None;
    }
    relative_path(folder)
        .into_iter()
        .chain([Path::new(""), Path::new("Expressions"), Path::new("exp")])
        .map(|directory| directory.join(file))
        .find(|candidate| {
            *cache.entry(candidate.clone()).or_insert_with(|| {
                root.join(candidate)
                    .canonicalize()
                    .ok()
                    .filter(|path| path.starts_with(root))
                    .and_then(|path| read_json(&path, MAX_EXPRESSION_BYTES).ok())
                    .is_some_and(|json| valid_expression(&json))
            })
        })
}

fn valid_expression(expression: &Value) -> bool {
    expression["Parameters"].as_array().is_some_and(|params| {
        params.iter().any(|param| {
            text(&param["Id"], MAX_TEXT_BYTES).is_some()
                && param["Value"]
                    .as_f64()
                    .is_some_and(|value| (value as f32).is_finite())
                && matches!(
                    param["Blend"].as_str().unwrap_or(""),
                    "" | "Add" | "Multiply" | "Overwrite"
                )
        })
    })
}

fn read_json(path: &Path, limit: u64) -> io::Result<Value> {
    let metadata = fs::metadata(path)?;
    if !metadata.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "expected a regular file",
        ));
    }
    let too_large = || {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("file exceeds {limit} bytes"),
        )
    };
    if metadata.len() > limit {
        return Err(too_large());
    }
    let mut bytes = Vec::new();
    File::open(path)?.take(limit + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > limit {
        return Err(too_large());
    }
    let input = bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&bytes);
    serde_json::from_slice(input).map_err(|err| io::Error::new(io::ErrorKind::InvalidData, err))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::sync::atomic::{AtomicUsize, Ordering};

    const EXPRESSION: &str = r#"{"Parameters":[{"Id":"ParamAngleX","Value":1,"Blend":"Add"}]}"#;

    struct Fixture(PathBuf);

    impl Fixture {
        fn new() -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            loop {
                let path = std::env::temp_dir().join(format!(
                    "deskwaifu-vtube-{}-{}",
                    std::process::id(),
                    NEXT.fetch_add(1, Ordering::Relaxed)
                ));
                match fs::create_dir(&path) {
                    Ok(()) => return Self(path),
                    Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                    Err(error) => panic!("create fixture: {error}"),
                }
            }
        }

        fn write(&self, name: &str, bytes: impl AsRef<[u8]>) {
            let path = self.0.join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        }

        fn config(&self, rows: Vec<Value>) {
            self.write("sample.vtube.json", json!({ "Hotkeys": rows }).to_string());
        }

        fn load(&self) -> Result<Vec<Shortcut>, String> {
            load(&self.0.join("sample.model3.json"))
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn binding(id: &str, file: &str) -> Value {
        json!({
            "HotkeyID": id,
            "Action": "ToggleExpression",
            "File": file,
        })
    }

    #[test]
    fn matches_only_the_selected_model_and_preserves_legacy_metadata() {
        let fixture = Fixture::new();
        let name = "\u{8138}\u{7ea2}.exp3.json";
        fixture.write(&format!("exp/{name}"), EXPRESSION);
        let mut row = binding("blush", name);
        row["Triggers"] = json!({"Trigger3": "Tab", "Trigger1": "LeftShift", "Trigger2": ""});
        row["IsGlobal"] = json!(true);
        row["DeactivateAfterSeconds"] = json!(true);
        row["DeactivateAfterSecondsAmount"] = json!(1.25);
        fixture.config(vec![row]);
        assert!(
            load(&fixture.0.join("other.model3.json"))
                .unwrap()
                .is_empty()
        );
        assert!(load(&fixture.0.join("sample.json")).unwrap().is_empty());
        let shortcuts = fixture.load().unwrap();
        let [shortcut] = shortcuts.as_slice() else {
            panic!("expected one shortcut")
        };
        assert_eq!(shortcut.id, "blush");
        assert_eq!(shortcut.name, name);
        assert_eq!(shortcut.keys, ["LeftShift", "Tab"]);
        assert_eq!(shortcut.action, "ToggleExpression");
        assert!(shortcut.enabled);
        assert_eq!(
            shortcut.expression,
            Some(PathBuf::from(format!("exp/{name}")))
        );
        assert_eq!(shortcut.fade, Duration::from_millis(500));
        assert_eq!(shortcut.duration, Some(Duration::from_millis(1250)));
        assert_eq!(shortcut.unsupported_reason, None);
    }

    #[test]
    fn lists_unsupported_rows_without_blocking_valid_buttons() {
        let fixture = Fixture::new();
        fixture.write("face.exp3.json", EXPRESSION);
        let mut valid = binding("valid", "face.exp3.json");
        valid["Triggers"] = json!({"Trigger1": "unknown-key", "Trigger2": 42});
        let mut disabled = binding("disabled", "face.exp3.json");
        disabled["IsActive"] = json!(false);
        let mut release = binding("release", "face.exp3.json");
        release["DeactivateAfterKeyUp"] = json!(true);
        let mut action = binding("motion", "face.exp3.json");
        action["Action"] = json!("TriggerAnimation");
        fixture.config(vec![
            valid,
            disabled,
            release,
            action,
            binding("valid", "face.exp3.json"),
            binding("", "face.exp3.json"),
            binding("missing", "missing.exp3.json"),
        ]);

        let shortcuts = fixture.load().unwrap();
        assert_eq!(shortcuts[0].keys, ["unknown-key", "<invalid trigger>"]);
        assert!(shortcuts[0].enabled);
        assert!(!shortcuts[1].enabled);
        assert_eq!(shortcuts[0].duration, None);
        assert_eq!(shortcuts[3].action, "TriggerAnimation");
        assert_eq!(
            shortcuts
                .iter()
                .map(|item| item.unsupported_reason.as_deref())
                .collect::<Vec<_>>(),
            [
                None,
                None,
                Some("Release-to-deactivate is not supported"),
                Some("Unsupported action"),
                Some("Missing, invalid or duplicate hotkey ID"),
                Some("Missing, invalid or duplicate hotkey ID"),
                Some("Expression file missing or invalid"),
            ]
        );
        assert!(shortcuts[2..].iter().all(|item| item.expression.is_none()));
    }

    #[test]
    fn resolves_folder_first_then_valid_fallbacks() {
        let fixture = Fixture::new();
        fixture.write("custom/nested/face.exp3.json", EXPRESSION);
        fixture.write("face.exp3.json", EXPRESSION);
        fixture.write("custom/broken.exp3.json", "{}");
        fixture.write("Expressions/broken.exp3.json", EXPRESSION);
        let mut custom = binding("custom", "face.exp3.json");
        custom["Folder"] = json!("custom/nested");
        let mut fallback = binding("fallback", "broken.exp3.json");
        fallback["Folder"] = json!("custom");
        fixture.config(vec![custom, fallback]);
        let shortcuts = fixture.load().unwrap();
        assert_eq!(
            shortcuts[0].expression,
            Some("custom/nested/face.exp3.json".into())
        );
        assert_eq!(
            shortcuts[1].expression,
            Some("Expressions/broken.exp3.json".into())
        );
    }

    #[test]
    fn rejects_traversal_absolute_and_non_expression_paths() {
        let fixture = Fixture::new();
        fixture.write("face.exp3.json", EXPRESSION);
        fixture.write("face.json", EXPRESSION);
        let absolute = fixture.0.join("face.exp3.json");
        fixture.config(vec![
            binding("traversal", "x/../face.exp3.json"),
            binding("absolute", absolute.to_str().unwrap()),
            binding("windows", "C:\\face.exp3.json"),
            binding("nul", "face\0.exp3.json"),
            binding("wrong-extension", "face.json"),
        ]);
        assert!(
            fixture
                .load()
                .unwrap()
                .iter()
                .all(|item| item.expression.is_none())
        );
    }

    #[cfg(unix)]
    #[test]
    fn refuses_sidecar_and_expression_symlinks_outside_model_directory() {
        use std::os::unix::fs::symlink;

        let outside = Fixture::new();
        outside.write("outside.exp3.json", EXPRESSION);
        outside.write("outside.vtube.json", r#"{"Hotkeys":[]}"#);
        let fixture = Fixture::new();
        symlink(
            outside.0.join("outside.vtube.json"),
            fixture.0.join("sample.vtube.json"),
        )
        .unwrap();
        assert!(
            fixture
                .load()
                .unwrap_err()
                .contains("within the model directory")
        );
        let model = Fixture::new();
        symlink(&outside.0, model.0.join("exp")).unwrap();
        model.config(vec![binding("linked", "outside.exp3.json")]);
        assert_eq!(model.load().unwrap()[0].expression, None);
    }

    #[test]
    fn bounds_payloads_rows_and_timing_values() {
        let fixture = Fixture::new();
        fixture.write(
            "sample.vtube.json",
            vec![b' '; MAX_CONFIG_BYTES as usize + 1],
        );
        assert!(fixture.load().unwrap_err().contains("file exceeds"));
        fixture.config(vec![Value::Null; MAX_SHORTCUTS + 1]);
        assert!(fixture.load().unwrap_err().contains("at most"));
        fixture.write("sample.vtube.json", "{invalid");
        assert!(fixture.load().is_err());
        fixture.write("sample.vtube.json", r#"{"Hotkeys":{}}"#);
        assert!(
            fixture
                .load()
                .unwrap_err()
                .contains("missing Hotkeys array")
        );

        fixture.write(
            "too-large.exp3.json",
            vec![b' '; MAX_EXPRESSION_BYTES as usize + 1],
        );
        let mut row = binding("timed", "too-large.exp3.json");
        row["FadeSecondsAmount"] = json!(1e100);
        row["DeactivateAfterSeconds"] = json!(true);
        row["DeactivateAfterSecondsAmount"] = json!("not numeric");
        let mut negative = binding("negative", "too-large.exp3.json");
        negative["DeactivateAfterSeconds"] = json!(true);
        negative["DeactivateAfterSecondsAmount"] = json!(-1);
        fixture.config(vec![row, negative]);
        let shortcuts = fixture.load().unwrap();
        assert_eq!(shortcuts[0].expression, None);
        assert_eq!(shortcuts[0].fade, Duration::from_secs_f64(MAX_SECONDS));
        assert_eq!(shortcuts[0].duration, Some(Duration::from_secs(10)));
        assert_eq!(shortcuts[1].duration, Some(Duration::ZERO));
    }
}
