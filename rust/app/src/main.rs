mod render;

use deskwaifu_control_domain::{
    Arbiter, Channel, ChannelValue, ControlError, ModelIdentity, SourceId,
};
use deskwaifu_cubism_core::{Moc, Model};
use deskwaifu_hotkey_domain::{FsReader, Hotkeys};
use deskwaifu_ipc::{ConnectionId, IpcServer};
use deskwaifu_model_domain::catalog::{scan_model_catalog, ModelDescriptor};
use deskwaifu_model_domain::model3::Model3;
use render::ViewState;
use serde_json::{json, Value};
use std::env;
use std::fs;
use std::io;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

struct Entry {
    descriptor: ModelDescriptor,
    /// The parsed descriptor, so metadata commands never re-read or re-parse it.
    model3: Model3,
    moc_path: PathBuf,
}

struct Loaded {
    entry: usize,
    _moc: Moc,
    model: Model,
    /// Hotkeys and persistent expression state for `entry`; an unloaded instance
    /// (empty token) until the model becomes current.
    hotkeys: Hotkeys,
}

/// Everything the request handler needs about presentation: the transform the IPC
/// layer owns and the rendering session, which is `None` in headless mode.
/// Grouped so `handle` takes one argument instead of growing one per feature.
struct Presentation {
    view: View,
    session: Option<render::Session>,
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

impl View {
    /// The same transform, in the shape the rendering session consumes.
    fn render_state(&self) -> render::ViewState {
        render::ViewState {
            zoom: self.zoom,
            x: self.x,
            y: self.y,
            hidden: self.hidden,
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
    let json = fs::read_to_string(&descriptor.id).ok()?;
    let model3 = Model3::parse(&json).ok()?;
    let moc_path = Path::new(&descriptor.directory).join(model3.model_file_name());
    if !moc_path.is_file() {
        return None;
    }
    Some(Entry {
        descriptor,
        model3,
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
        hotkeys: Hotkeys::new(),
    })
}

/// Reads the model's `.vtube.json` next to its `.model3.json`, as the C++
/// `LAppModel::Load` does. Every call issues a fresh model token, even when the
/// file is absent, so a reply carrying an earlier token is recognizably stale.
fn load_hotkeys(entry: usize, entries: &[Entry]) -> Hotkeys {
    let descriptor = &entries[entry].descriptor;
    let mut hotkeys = Hotkeys::new();
    hotkeys.load(
        &FsReader::new(Path::new(&descriptor.directory)),
        &descriptor.json_name,
    );
    hotkeys
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
///
/// A switch to a different model reloads its hotkeys, so the model token changes
/// with the model. Re-selecting the current model is not a scene change; the C++
/// `ChangeScene` returns early there, so the loaded hotkeys and their token and
/// expression state are kept.
///
/// In overlay mode a real switch also rebuilds the rendered model, its textures
/// and its renderer. A rendering failure answers the request with the error: the
/// core model has already switched, so claiming success would hide a window that
/// silently stopped drawing.
fn select(
    index: usize,
    entries: &[Entry],
    current: &mut Loaded,
    controls: &mut Arbiter,
    session: &mut Option<render::Session>,
    view: ViewState,
) -> Result<Value, String> {
    let mut replacement = load(index, entries)?;
    // A real switch reloads the hotkeys, so the token changes with the model and
    // no expression state carries over; re-selecting the current model keeps them.
    replacement.hotkeys = if index == current.entry {
        std::mem::take(&mut current.hotkeys)
    } else {
        load_hotkeys(index, entries)
    };
    *current = replacement;
    controls.model_switch(ModelIdentity::new(
        index as i32,
        entries[index].descriptor.id.clone(),
    ));
    if let Some(session) = session.as_mut() {
        session.reload(&entries[index], view)?;
    }
    Ok(json!({"ok":true,"model":entries[index].descriptor.name}))
}

/// Mirrors the C++ `ModelRuntime::Metadata()` hotkey object. The C++ emits only
/// `id`, `name`, `action`, `file`, `reason`, `enabled`, `active` and `keys`; it
/// exposes neither fade nor timeout nor the resolved expression index.
fn hotkeys_reply(hotkeys: &Hotkeys) -> Value {
    let bindings = hotkeys
        .bindings()
        .iter()
        .map(|binding| {
            json!({
                "id": binding.id,
                "name": binding.name,
                "action": binding.action,
                "file": binding.file,
                "reason": binding.reason,
                "enabled": binding.enabled,
                "active": binding.reason.is_empty() && hotkeys.is_active(&binding.id),
                "keys": binding.keys,
            })
        })
        .collect::<Vec<_>>();
    json!({
        "ok": true,
        "model_token": hotkeys.token(),
        "warning": hotkeys.error(),
        "hotkeys": bindings,
    })
}

/// The C++ `trigger_hotkey` requires a token that names the current load.
///
/// Divergence: before dispatching, the C++ `Execute` applies a generic check to
/// every model operation and reports `Model changed; stale operation discarded`
/// for a stale non-empty token, so `trigger_hotkey` reaches its own
/// `Model changed; refresh hotkeys before triggering` only for an empty token.
/// This runtime reports the trigger-specific text for both, which is what clients
/// act on: refetch `get_hotkeys` and retry.
fn current_token(args: &Value, current: &Loaded) -> Result<(), String> {
    match args.get("model_token").and_then(Value::as_str) {
        Some(token) if !token.is_empty() && token == current.hotkeys.token() => Ok(()),
        _ => Err("Model changed; refresh hotkeys before triggering".into()),
    }
}

fn handle(
    request: &[u8],
    connection: ConnectionId,
    entries: &[Entry],
    current: &mut Loaded,
    controls: &mut Arbiter,
    presentation: &mut Presentation,
    quit: &mut bool,
) -> Vec<u8> {
    let Presentation { view, session } = presentation;
    let result = (|| -> Result<Value, String> {
        let request: Value = serde_json::from_slice(request).map_err(|e| e.to_string())?;
        let command = request
            .get("command")
            .and_then(Value::as_str)
            .or_else(|| request.as_str())
            .ok_or("request requires string command")?;
        let args = request.get("args").unwrap_or(&request);
        match command {
            // The counts come from the runtime's own readback, so a renderer is
            // only reported when it really produced a frame with pixels in it.
            "get_status" => {
                let (available, stats) = session
                    .as_ref()
                    .map_or((false, None), render::Session::status);
                let mut reply = json!({
                    "ok": true,
                    "renderer_available": available,
                    "model_core_ready": true,
                    "model": entries[current.entry].descriptor.name,
                    "model_index": current.entry,
                    "model_count": entries.len(),
                    "hidden": view.hidden,
                    "zoom": view.zoom,
                    "x": view.x,
                    "y": view.y,
                });
                if let Some(stats) = stats {
                    reply["frames_drawn"] = json!(stats.frames_drawn);
                    reply["visible_pixels"] = json!(stats.visible_pixels);
                    reply["transparent_pixels"] = json!(stats.transparent_pixels);
                    reply["pixel_checksum"] = json!(stats.pixel_checksum);
                    reply["width"] = json!(stats.width);
                    reply["height"] = json!(stats.height);
                }
                // Only overlay mode adds rendering keys: the headless reply keeps
                // exactly the shape clients already see. An overlay start or frame
                // failure reports its text, so the window is never quietly blank.
                if let Some(session) = session.as_ref() {
                    reply["renderer_error"] = match session.error() {
                        Some(error) => json!(error),
                        None => Value::Null,
                    };
                }
                Ok(reply)
            }
            // A test/diagnostic seam: force one frame and a fresh readback so the
            // counts describe the model as it is drawn right now. With `id`, the
            // reply also reports the rendered model's current value for it.
            "debug_frame" => {
                let session = session
                    .as_mut()
                    .ok_or("renderer not available; start with --overlay")?;
                let mut reply = session
                    .stats(view.render_state())?
                    .to_json(session.available());
                if let Some(id) = args.get("id").and_then(Value::as_str) {
                    let value = session.parameter(id)?;
                    reply["parameter"] = json!(id);
                    reply["parameter_value"] = json!(value);
                }
                Ok(reply)
            }
            "get_available_models" => Ok(
                json!({"ok":true,"models":entries.iter().map(|e| &e.descriptor.name).collect::<Vec<_>>()}),
            ),
            "get_current_model" => {
                Ok(json!({"ok":true,"model":entries[current.entry].descriptor.name}))
            }
            // The C++ `get_motions` returns the model setting's motion list in
            // declaration order, which `Model3::motion_list` reproduces.
            "get_motions" => {
                let motions = entries[current.entry]
                    .model3
                    .motion_list()
                    .into_iter()
                    .map(|motion| {
                        json!({"group":motion.group,"index":motion.index,"file":motion.file})
                    })
                    .collect::<Vec<_>>();
                Ok(json!({"ok":true,"motions":motions}))
            }
            // The C++ lists every loaded expression, including `.exp3.json` files
            // it discovers by scanning the model directory. That scan is a
            // renderer-side load step; this runtime reports only the
            // `FileReferences.Expressions` names, the current boundary between
            // the two implementations.
            "get_expressions" => Ok(json!({
                "ok": true,
                "expressions": entries[current.entry].model3.expression_names(),
            })),
            "get_hotkeys" => Ok(hotkeys_reply(&current.hotkeys)),
            "trigger_hotkey" => {
                current_token(args, current)?;
                // The C++ reads `id` with `JsonGetString`, which yields `""` for a
                // missing or non-string field; `Trigger("")` then reports that the
                // hotkey is not in the current model.
                let id = args.get("id").and_then(Value::as_str).unwrap_or_default();
                current.hotkeys.trigger(id).map_err(|e| e.to_string())?;
                Ok(hotkeys_reply(&current.hotkeys))
            }
            // Applying an expression or motion goes through the Cubism animation
            // bridge, which this runtime does not drive yet. Answering `ok` would
            // claim a capability it lacks, so both commands fail explicitly.
            "set_expression" => Err(if session.is_some() {
                "expressions are not implemented by the Rust rendering runtime yet".into()
            } else {
                "renderer not available; expression changes require the rendering runtime".into()
            }),
            "do_motion" => Err(if session.is_some() {
                "motion commands are not implemented by the Rust rendering runtime yet".into()
            } else {
                "renderer not available; motion playback requires the rendering runtime".into()
            }),
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
                select(
                    index,
                    entries,
                    current,
                    controls,
                    session,
                    view.render_state(),
                )
            }
            "next_model" | "prev_model" => {
                let count = entries.len();
                let index = if command == "next_model" {
                    (current.entry + 1) % count
                } else {
                    (current.entry + count - 1) % count
                };
                select(
                    index,
                    entries,
                    current,
                    controls,
                    session,
                    view.render_state(),
                )
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
                // The Core model owns validation, the reply and `get_parameters`;
                // the out-of-range rejection therefore stays exactly as it was.
                current
                    .model
                    .set_parameter_value(index, value)
                    .map_err(|e| e.to_string())?;
                // The drawn frame reads the rendered model's own parameters, so the
                // same value is pushed there by ID; the reply keeps the Core value.
                if let Some(session) = session.as_mut() {
                    if let Some(parameter) = current.model.parameters().get(index) {
                        session.set_parameter(&parameter.id, value)?;
                    }
                }
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

/// `(models_dir, single_model, socket, overlay)`.
struct Arguments {
    root: Option<PathBuf>,
    single: Option<PathBuf>,
    socket: PathBuf,
    /// Opt-in rendering into a layer-shell window; absent means the unchanged
    /// headless runtime, which is what the existing tests and CI exercise.
    overlay: bool,
}

fn arguments() -> Result<Arguments, String> {
    let mut models = None;
    let mut model = None;
    let mut socket = None;
    let mut overlay = false;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--overlay" | "--render" => {
                if overlay {
                    return Err(format!("repeated argument: {arg}"));
                }
                overlay = true;
                continue;
            }
            _ => {}
        }
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
    Ok(Arguments {
        root: models,
        single: model,
        socket: socket.ok_or("--socket is required")?,
        overlay,
    })
}

fn run() -> Result<(), String> {
    let arguments = arguments()?;
    let (root, single, socket_path) = (arguments.root, arguments.single, arguments.socket);
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
    let mut initial = load(0, &entries)?;
    initial.hotkeys = load_hotkeys(0, &entries);
    let (mut server, _socket) =
        OwnedSocket::bind(&socket_path).map_err(|e| format!("cannot bind socket: {e}"))?;
    // Nothing is drawn until the compositor, the Framework and the first model
    // load all succeed; a failure here is a runtime failure with text.
    let session = if arguments.overlay {
        Some(render::Session::start(&entries[0]).map_err(|e| format!("overlay mode failed: {e}"))?)
    } else {
        None
    };
    let mut presentation = Presentation {
        view: View::default(),
        session,
    };
    let mut current = initial;
    let mut controls = Arbiter::new(ModelIdentity::new(0, entries[0].descriptor.id.clone()));
    let mut quit = false;
    let mut next_update = Instant::now();
    while !quit {
        if Instant::now() >= next_update {
            current.model.update();
            // Same 16 ms frame step the C++ runtime uses for both the model and
            // its hotkeys, so expression fades and timeouts advance together.
            current.hotkeys.update(0.016);
            if let Some(session) = presentation.session.as_mut() {
                session.frame(presentation.view.render_state());
            }
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
                        &mut presentation,
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
