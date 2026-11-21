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
