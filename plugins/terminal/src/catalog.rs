use serde::Serialize;
use std::env;
use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

const MAX_DEPTH: usize = 8;
const MAX_ENTRIES: usize = 10_000;
// The host wraps plugin output in a string field in its 64 KiB JSON IPC frame.
const MAX_FRAME_BYTES: usize = 64 * 1024;
const RESPONSE_OVERHEAD: usize = r#"{"status":"ok","output":""}"#.len();

#[derive(Serialize)]
struct Catalog {
    models: Vec<String>,
}

pub(super) fn response(
    directory: Option<&str>,
    configured_root: Option<&str>,
) -> Result<String, String> {
    serialize(&Catalog {
        models: models(directory.or(configured_root))?,
    })
}

pub(super) fn models(directory: Option<&str>) -> Result<Vec<String>, String> {
    let (root, allow_missing) = catalog_root(directory, |name| env::var_os(name))?;
    discover(&root, allow_missing, MAX_DEPTH, MAX_ENTRIES)
}

fn serialize(catalog: &Catalog) -> Result<String, String> {
    let output = serde_json::to_string(catalog)
        .map_err(|error| format!("terminal catalog serialization failed: {error}"))?;
    // The inner JSON has already escaped control characters. Only quotes and backslashes
    // take an extra byte when encoding it as the host's output string.
    let escapes = output
        .bytes()
        .filter(|&byte| matches!(byte, b'"' | b'\\'))
        .count();
    if RESPONSE_OVERHEAD + output.len() + escapes > MAX_FRAME_BYTES {
        return Err(
            "terminal catalog: response exceeds 64 KiB; specify a smaller directory".into(),
        );
    }
    Ok(output)
}

fn catalog_root(
    directory: Option<&str>,
    get_env: impl Fn(&str) -> Option<OsString>,
) -> Result<(PathBuf, bool), String> {
    if let Some(directory) = directory {
        return Ok((directory.into(), false));
    }
    if let Some(directory) = get_env("DESKWAIFU_MODEL_DIR").filter(|dir| !dir.is_empty()) {
        return Ok((directory.into(), false));
    }
    let absolute_env_path = |name| {
        get_env(name)
            .map(PathBuf::from)
            .filter(|path| path.is_absolute())
    };
    let data_home = absolute_env_path("XDG_DATA_HOME")
        .or_else(|| absolute_env_path("HOME").map(|home| home.join(".local/share")))
        .ok_or("terminal catalog: specify a directory or set DESKWAIFU_MODEL_DIR, XDG_DATA_HOME or HOME")?;
    Ok((data_home.join("deskwaifu/models"), true))
}

fn discover(
    root: &Path,
    allow_missing: bool,
    max_depth: usize,
    max_entries: usize,
) -> Result<Vec<String>, String> {
    let root = match fs::canonicalize(root) {
        Ok(root) => root,
        Err(error) if allow_missing && error.kind() == io::ErrorKind::NotFound => {
            return Ok(Vec::new());
        }
        Err(error) => return Err(scan_error(root, error)),
    };
    let mut models = Vec::new();
    let mut remaining = max_entries;
    let mut directories = vec![(root.clone(), 0)];
    while let Some((directory, depth)) = directories.pop() {
        let entries = fs::read_dir(&directory).map_err(|error| scan_error(&directory, error))?;
        for entry in entries {
            if remaining == 0 {
                return Err(format!(
                    "terminal catalog: entry limit ({max_entries}) exceeded"
                ));
            }
            remaining -= 1;
            let entry = entry.map_err(|error| scan_error(&directory, error))?;
            let path = entry.path();
            let file_type = entry
                .file_type()
                .map_err(|error| scan_error(&path, error))?;
            match file_type {
                kind if kind.is_dir() && depth < max_depth => directories.push((path, depth + 1)),
                kind if kind.is_file() => {
                    // Keep only paths that can be passed back to the CLI verbatim.
                    if let Some(path) = path.to_str().filter(|path| path.ends_with(".model3.json"))
                    {
                        models.push(path.to_owned());
                    }
                }
                _ => {}
            }
        }
    }
    models.sort_unstable();
    Ok(models)
}

fn scan_error(path: &Path, error: io::Error) -> String {
    format!("terminal catalog: {}: {error}", path.display())
}

#[cfg(test)]
mod tests;
