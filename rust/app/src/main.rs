use deskwaifu_cubism_core::{Moc, Model};
use deskwaifu_ipc::IpcServer;
use serde_json::{json, Value};
use std::env;
use std::fs;
use std::io;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

struct Entry {
    name: String,
    moc_path: PathBuf,
}

struct Loaded {
    entry: usize,
    _moc: Moc,
    model: Model,
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

fn model_entry(path: &Path) -> Option<Entry> {
    let value: Value = serde_json::from_slice(&fs::read(path).ok()?).ok()?;
    let moc = value.get("FileReferences")?.get("Moc")?.as_str()?;
    let moc_path = path.parent()?.join(moc);
    if !moc_path.is_file() {
        return None;
    }
    let name = value
        .get("Name")
        .and_then(Value::as_str)
        .map(str::to_owned)
        .or_else(|| path.file_stem()?.to_str().map(str::to_owned))?;
    Some(Entry { name, moc_path })
}

fn discover(root: &Path) -> Vec<Entry> {
    fn walk(dir: &Path, output: &mut Vec<Entry>) {
        let Ok(items) = fs::read_dir(dir) else { return };
        for item in items.flatten() {
            let path = item.path();
            if path.is_dir() {
                walk(&path, output);
            } else if path
                .file_name()
                .is_some_and(|n| n.to_string_lossy().ends_with(".model3.json"))
            {
                if let Some(entry) = model_entry(&path) {
                    output.push(entry);
                }
            }
        }
    }
    let mut entries = Vec::new();
    walk(root, &mut entries);
    entries.sort_by(|a, b| a.name.cmp(&b.name));
    entries
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
fn handle(request: &[u8], entries: &[Entry], current: &mut Loaded, quit: &mut bool) -> Vec<u8> {
    let result = (|| -> Result<Value, String> {
        let request: Value = serde_json::from_slice(request).map_err(|e| e.to_string())?;
        let command = request
            .get("command")
            .and_then(Value::as_str)
            .or_else(|| request.as_str())
            .ok_or("request requires string command")?;
        let args = request.get("args").unwrap_or(&request);
        match command {
            "get_status" => Ok(
                json!({"ok":true,"renderer_available":false,"model_core_ready":true,"current_model":entries[current.entry].name}),
            ),
            "get_available_models" => {
                Ok(json!({"ok":true,"models":entries.iter().map(|e| &e.name).collect::<Vec<_>>()}))
            }
            "get_current_model" => Ok(json!({"ok":true,"model":entries[current.entry].name})),
            "set_model" => {
                let selector = args
                    .get("name")
                    .or_else(|| args.get("index"))
                    .ok_or("set_model requires name or index")?;
                let index = if let Some(name) = selector.as_str() {
                    entries
                        .iter()
                        .position(|e| e.name == name)
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
                let replacement = load(index, entries)?;
                *current = replacement;
                Ok(json!({"ok":true,"model":entries[index].name}))
            }
            "get_parameters" => {
                let params = current.model.parameters().iter().enumerate().map(|(i,p)| json!({"index":i,"id":p.id,"type":p.parameter_type,"minimum":p.minimum,"maximum":p.maximum,"default":p.default,"value":current.model.parameter_value(i).unwrap_or(p.default)})).collect::<Vec<_>>();
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
        vec![model_entry(&path).ok_or("invalid model3.json or missing referenced Moc")?]
    } else {
        let path = root.unwrap();
        if !path.is_dir() {
            return Err("models directory does not exist".into());
        }
        discover(&path)
    };
    if entries.is_empty() {
        return Err("no valid model3.json with referenced Moc found".into());
    }
    let initial = load(0, &entries)?;
    let (mut server, _socket) =
        OwnedSocket::bind(&socket_path).map_err(|e| format!("cannot bind socket: {e}"))?;
    let mut current = initial;
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
        server
            .poll_once(Some(timeout), |request| {
                handle(request, &entries, &mut current, &mut quit)
            })
            .map_err(|e| e.to_string())?;
    }
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        eprintln!("deskwaifu-rust: {e}");
        std::process::exit(1);
    }
}
