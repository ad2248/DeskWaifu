use deskwaifu_control_domain::{
    Arbiter, Channel, ChannelValue, ControlError, ModelIdentity, SourceId,
};
use deskwaifu_cubism_core::{Moc, Model};
use deskwaifu_ipc::{ConnectionId, IpcServer};
use deskwaifu_model_domain::catalog::{scan_model_catalog, ModelDescriptor};
use serde_json::{json, Value};
use std::env;
use std::fs;
use std::io;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

struct Entry {
    descriptor: ModelDescriptor,
    moc_path: PathBuf,
}

struct Loaded {
    entry: usize,
    _moc: Moc,
    model: Model,
}

/// Display state the desktop UI would otherwise own: the model transform and
/// window visibility. It stays in memory for the lifetime of the process.
struct View {
    zoom: f32,
    x: f32,
    y: f32,
    hidden: bool,
}

impl Default for View {
    fn default() -> Self {
        // Same defaults as the C++ `ModelTransform` and `AppSnapshot`.
        Self {
            zoom: 1.0,
            x: 0.0,
            y: 0.0,
            hidden: false,
        }
    }
}

struct OwnedSocket {
    path: PathBuf,
    dev: u64,
    ino: u64,
}

impl OwnedSocket {
    fn bind(path: &Path) -> io::Result<(IpcServer, Self)> {
        if fs::symlink_metadata(path).is_ok() {
            return Err(io::Error::new(
                io::ErrorKind::AddrInUse,
                "socket path already exists",
            ));
        }
        let server = IpcServer::bind(path)?;
        let metadata = fs::symlink_metadata(path)?;
        Ok((
            server,
            Self {
                path: path.to_owned(),
                dev: metadata.dev(),
                ino: metadata.ino(),
            },
        ))
    }
}
impl Drop for OwnedSocket {
    fn drop(&mut self) {
        if let Ok(metadata) = fs::symlink_metadata(&self.path) {
            if metadata.dev() == self.dev && metadata.ino() == self.ino {
                let _ = fs::remove_file(&self.path);
            }
        }
    }
}

fn model_entry(descriptor: ModelDescriptor) -> Option<Entry> {
    let value: Value = serde_json::from_slice(&fs::read(&descriptor.id).ok()?).ok()?;
    let moc = value.get("FileReferences")?.get("Moc")?.as_str()?;
    let moc_path = Path::new(&descriptor.directory).join(moc);
    if !moc_path.is_file() {
        return None;
    }
    Some(Entry {
        descriptor,
        moc_path,
    })
}

fn single_descriptor(path: &Path) -> Option<ModelDescriptor> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let directory = fs::canonicalize(parent).ok()?;
    let name = directory.file_name()?.to_string_lossy().into_owned();
    let json_name = path.file_name()?.to_string_lossy().into_owned();
    let mut directory = directory.to_string_lossy().into_owned();
    if !directory.ends_with('/') {
        directory.push('/');
    }
    Some(ModelDescriptor {
        id: format!("{directory}{json_name}"),
        name,
        directory,
        json_name,
    })
}

fn load(entry: usize, entries: &[Entry]) -> Result<Loaded, String> {
    let bytes = fs::read(&entries[entry].moc_path).map_err(|e| e.to_string())?;
    let moc = Moc::from_bytes(&bytes).map_err(|e| e.to_string())?;
    let model = moc.create_model().map_err(|e| e.to_string())?;
    Ok(Loaded {
        entry,
        _moc: moc,
        model,
    })
}

fn error(message: impl ToString) -> Value {
    json!({"ok": false, "error": message.to_string()})
}

fn control_error(error: ControlError) -> String {
    match error {
        ControlError::MouthOutOfRange => "mouth must be finite and within 0..1".into(),
        ControlError::LookNotFinite => "look coordinates must be finite".into(),
    }
}

/// Reads an optional boolean field the way the C++ `JsonGetBool` does: a missing
/// field is false, a present field of another type is an error, and the legacy
/// `1`/`0` and `"true"`/`"false"`/`"1"`/`"0"` spellings stay accepted for
/// `reset` and `stream_source`.
fn bool_field(args: &Value, key: &str) -> Result<bool, String> {
    match args.get(key) {
        None => Ok(false),
        Some(Value::Bool(value)) => Ok(*value),
        Some(Value::Number(value)) => match value.as_i64() {
            Some(1) => Ok(true),
            Some(0) => Ok(false),
            _ => Err(format!("{key} must be boolean")),
        },
        Some(Value::String(value)) => match value.as_str() {
            "true" | "1" => Ok(true),
            "false" | "0" => Ok(false),
            _ => Err(format!("{key} must be boolean")),
        },
        Some(_) => Err(format!("{key} must be boolean")),
    }
}

/// Reads an optional float field the way the C++ `JsonGetFloat` does: a missing
/// field keeps the caller's fallback, while a present non-number or non-finite
/// value fails instead of silently converting.
fn float_field(args: &Value, key: &str, fallback: f32) -> Result<f32, String> {
    match args.get(key) {
        None => Ok(fallback),
        Some(value) => {
            let value = value
                .as_f64()
                .ok_or_else(|| format!("{key} must be a finite number"))?;
            if !value.is_finite() || !(value as f32).is_finite() {
                return Err(format!("{key} must be a finite number"));
            }
            Ok(value as f32)
        }
    }
}

/// Whether the request names the connection-owned streaming source instead of
/// the shared persistent one. The C++ handler rejects `stream_source` when the
/// request has no connection; every request here arrives through `poll_with`,
/// which always supplies a `ConnectionId`, so that failure cannot occur.
fn source_of(args: &Value, connection: ConnectionId) -> Result<SourceId, String> {
    Ok(if bool_field(args, "stream_source")? {
        SourceId::Connection(connection)
    } else {
        SourceId::Persistent
    })
}

/// Loads `index` and makes it current. A failed load returns before the state
/// changes, so the previous model and its controls stay intact; a successful
/// load reports the new identity, which clears external controls only when it
/// really differs.
fn select(
    index: usize,
    entries: &[Entry],
    current: &mut Loaded,
    controls: &mut Arbiter,
) -> Result<Value, String> {
    let replacement = load(index, entries)?;
    *current = replacement;
    controls.model_switch(ModelIdentity::new(
        index as i32,
        entries[index].descriptor.id.clone(),
    ));
    Ok(json!({"ok":true,"model":entries[index].descriptor.name}))
}

fn handle(
    request: &[u8],
    connection: ConnectionId,
    entries: &[Entry],
    current: &mut Loaded,
    controls: &mut Arbiter,
    view: &mut View,
    quit: &mut bool,
) -> Vec<u8> {
    let result = (|| -> Result<Value, String> {
        let request: Value = serde_json::from_slice(request).map_err(|e| e.to_string())?;
        let command = request
            .get("command")
            .and_then(Value::as_str)
            .or_else(|| request.as_str())
            .ok_or("request requires string command")?;
        let args = request.get("args").unwrap_or(&request);
        match command {
            "get_status" => Ok(json!({
                "ok": true,
                "renderer_available": false,
                "model_core_ready": true,
                "model": entries[current.entry].descriptor.name,
                "model_index": current.entry,
                "model_count": entries.len(),
                "hidden": view.hidden,
                "zoom": view.zoom,
                "x": view.x,
                "y": view.y,
            })),
            "get_available_models" => Ok(
                json!({"ok":true,"models":entries.iter().map(|e| &e.descriptor.name).collect::<Vec<_>>()}),
            ),
            "get_current_model" => {
                Ok(json!({"ok":true,"model":entries[current.entry].descriptor.name}))
            }
            "set_model" => {
                let selector = args
                    .get("name")
                    .or_else(|| args.get("index"))
                    .ok_or("set_model requires name or index")?;
                let index = if let Some(name) = selector.as_str() {
                    entries
                        .iter()
                        .position(|e| e.descriptor.name == name)
                        .ok_or("unknown model name")?
                } else {
                    let i = selector
                        .as_u64()
                        .ok_or("model index must be a nonnegative integer")?
                        as usize;
                    if i >= entries.len() {
                        return Err("model index out of bounds".into());
                    }
                    i
                };
                // Only a real identity change clears external controls; re-selecting
                // the current model leaves every source untouched.
                select(index, entries, current, controls)
            }
            "next_model" | "prev_model" => {
                let count = entries.len();
                let index = if command == "next_model" {
                    (current.entry + 1) % count
                } else {
                    (current.entry + count - 1) % count
                };
                select(index, entries, current, controls)
            }
            "get_model_position" => Ok(json!({"ok":true,"x":view.x,"y":view.y,"zoom":view.zoom})),
            "set_model_position" => {
                let x = float_field(args, "x", view.x)?;
                let y = float_field(args, "y", view.y)?;
                view.x = x;
                view.y = y;
                Ok(json!({"ok":true}))
            }
            "set_model_zoom" => {
                let zoom = float_field(args, "value", -1.0)?;
                if !(0.1..=10.0).contains(&zoom) {
                    return Err("value must be 0.1..10.0".into());
                }
                view.zoom = zoom;
                Ok(json!({"ok":true}))
            }
            "toggle_hidden" => {
                view.hidden = !view.hidden;
                Ok(json!({"ok":true,"hidden":view.hidden}))
            }
            "get_parameters" => {
                let params = current
                    .model
                    .parameters()
                    .iter()
                    .enumerate()
                    .map(|(i, p)| {
                        let value = current.model.parameter_value(i).map_err(|e| e.to_string())?;
                        Ok(json!({"index":i,"id":p.id,"type":p.parameter_type,"minimum":p.minimum,"maximum":p.maximum,"default":p.default,"value":value}))
                    })
                    .collect::<Result<Vec<_>, String>>()?;
                Ok(json!({"ok":true,"parameters":params}))
            }
            "set_parameter" => {
                let index = args
                    .get("index")
                    .and_then(Value::as_u64)
                    .ok_or("set_parameter requires nonnegative index")?
                    as usize;
                let value =
                    args.get("value")
                        .and_then(Value::as_f64)
                        .ok_or("set_parameter requires numeric value")? as f32;
                current
                    .model
                    .set_parameter_value(index, value)
                    .map_err(|e| e.to_string())?;
                Ok(
                    json!({"ok":true,"index":index,"value":current.model.parameter_value(index).map_err(|e| e.to_string())?}),
                )
            }
            "set_mouth_y" => {
                let source = source_of(args, connection)?;
                if bool_field(args, "reset")? {
                    controls.reset(source, Channel::Mouth);
                    return Ok(json!({"ok":true}));
                }
                // A missing value means 0.0, like the reference handler's
                // `JsonGetFloat(request, "value", 0.0f)`; a present non-number or
                // non-finite value fails without touching the state.
                let value = args.get("value").map_or(Ok(0.0), |value| {
                    value.as_f64().ok_or("value must be a finite number")
                })?;
                if !value.is_finite() {
                    return Err("value must be a finite number".into());
                }
                let value = value.clamp(0.0, 1.0) as f32;
                controls
                    .update(source, ChannelValue::Mouth(value))
                    .map_err(control_error)?;
                Ok(json!({"ok":true}))
            }
            "set_look" => {
                let source = source_of(args, connection)?;
                if bool_field(args, "reset")? {
                    controls.reset(source, Channel::Look);
                    return Ok(json!({"ok":true}));
                }
                let x = args.get("x").map_or(Ok(0.0), |value| {
                    value.as_f64().ok_or("x must be a finite number")
                })? as f32;
                let y = args.get("y").map_or(Ok(0.0), |value| {
                    value.as_f64().ok_or("y must be a finite number")
                })? as f32;
                controls
                    .update(source, ChannelValue::Look(x, y))
                    .map_err(control_error)?;
                Ok(json!({"ok":true}))
            }
            "release_source" => {
                controls.release(source_of(args, connection)?);
                Ok(json!({"ok":true}))
            }
            "get_control_status" => {
                let status = controls.status();
                Ok(json!({
                    "ok": true,
                    "has_mouth": status.has_mouth,
                    "mouth": status.mouth,
                    "has_look": status.has_look,
                    "look_x": status.look_x,
                    "look_y": status.look_y,
                }))
            }
            "quit" => {
                *quit = true;
                Ok(json!({"ok":true}))
            }
            other => Err(format!("unsupported command: {other}")),
        }
    })();
    serde_json::to_vec(&result.unwrap_or_else(error))
        .unwrap_or_else(|_| b"{\"ok\":false,\"error\":\"serialization failed\"}".to_vec())
}

fn arguments() -> Result<(Option<PathBuf>, Option<PathBuf>, PathBuf), String> {
    let mut models = None;
    let mut model = None;
    let mut socket = None;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {arg}"))?;
        match arg.as_str() {
            "--models-dir" if models.is_none() => models = Some(PathBuf::from(value)),
            "--model" if model.is_none() => model = Some(PathBuf::from(value)),
            "--socket" if socket.is_none() => socket = Some(PathBuf::from(value)),
            _ => return Err(format!("unknown or repeated argument: {arg}")),
        }
    }
    if models.is_some() == model.is_some() {
        return Err("provide exactly one of --models-dir or --model".into());
    }
    Ok((models, model, socket.ok_or("--socket is required")?))
}

fn run() -> Result<(), String> {
    let (root, single, socket_path) = arguments()?;
    let entries = if let Some(path) = single {
        vec![single_descriptor(&path)
            .and_then(model_entry)
            .ok_or("invalid model3.json or missing referenced Moc")?]
    } else {
        let path = root.unwrap();
        if !path.is_dir() {
            return Err("models directory does not exist".into());
        }
        scan_model_catalog(&[path])
            .into_iter()
            .filter_map(model_entry)
            .collect()
    };
    if entries.is_empty() {
        return Err("no valid model3.json with referenced Moc found".into());
    }
    let initial = load(0, &entries)?;
    let (mut server, _socket) =
        OwnedSocket::bind(&socket_path).map_err(|e| format!("cannot bind socket: {e}"))?;
    let mut current = initial;
    let mut controls = Arbiter::new(ModelIdentity::new(0, entries[0].descriptor.id.clone()));
    let mut view = View::default();
    let mut quit = false;
    let mut next_update = Instant::now();
    while !quit {
        if Instant::now() >= next_update {
            current.model.update();
            next_update += Duration::from_millis(16);
        }
        let timeout = next_update
            .saturating_duration_since(Instant::now())
            .min(Duration::from_millis(16));
        let mut disconnected = Vec::new();
        server
            .poll_with(
                Some(timeout),
                |connection, request| {
                    handle(
                        request,
                        connection,
                        &entries,
                        &mut current,
                        &mut controls,
                        &mut view,
                        &mut quit,
                    )
                },
                |connection| disconnected.push(connection),
            )
            .map_err(|e| e.to_string())?;
        // `poll_with` reports every disconnect after dispatching this round's
        // requests, so releasing the sources here keeps that same order.
        for connection in disconnected {
            controls.disconnect(connection);
        }
    }
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        eprintln!("deskwaifu-rust: {e}");
        std::process::exit(1);
    }
}
