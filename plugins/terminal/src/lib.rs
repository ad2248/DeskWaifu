//! CLI control for the P1 terminal overlay.

use deskwaifu_sdk::{
    Context, Message, PanelAction, PanelQuery, PanelSection, PanelTab, Plugin, PluginResult,
};
use serde::Serialize;

mod catalog;
#[cfg(all(target_os = "linux", feature = "native"))]
mod compositor;
#[cfg(all(target_os = "linux", feature = "kwin-cursor"))]
mod kwin_cursor;

#[cfg(all(target_os = "linux", feature = "native"))]
fn global_gaze() -> Option<layer_shell_probe::GlobalGaze> {
    #[cfg(feature = "kwin-cursor")]
    if std::env::var("XDG_CURRENT_DESKTOP").is_ok_and(|desktop| {
        desktop
            .split(':')
            .any(|name| name.eq_ignore_ascii_case("KDE"))
    }) {
        let mut provider = None;
        return Some(Box::new(move |connector, (width, height)| {
            let cursor = provider.get_or_insert_with(|| kwin_cursor::KWinCursor::start().ok());
            let cursor = cursor.as_ref()?;
            cursor.set_output(connector);
            cursor.local_position(width, height)
        }));
    }
    None
}
mod panel;
mod vtube;

#[cfg(all(target_os = "linux", feature = "native"))]
use deskwaifu_sdk::ConnectionId;
#[cfg(all(target_os = "linux", feature = "native"))]
use layer_shell_probe::{Command, Crop, Transform, Worker};

const INVALID_ARGUMENTS: &str = "invalid_arguments";
#[cfg(not(all(target_os = "linux", feature = "native")))]
const UNAVAILABLE: &str = "terminal native support is unavailable";

type ClipArgs = (i32, i32, u32, u32);

fn clip_args(args: &[String]) -> Result<Option<ClipArgs>, String> {
    match args {
        [] => Ok(None),
        [x, y, width, height] => {
            let x = x.parse().map_err(|_| INVALID_ARGUMENTS)?;
            let y = y.parse().map_err(|_| INVALID_ARGUMENTS)?;
            let width = width.parse().map_err(|_| INVALID_ARGUMENTS)?;
            let height = height.parse().map_err(|_| INVALID_ARGUMENTS)?;
            if width == 0 || height == 0 {
                return Err(INVALID_ARGUMENTS.into());
            }
            Ok(Some((x, y, width, height)))
        }
        _ => Err(INVALID_ARGUMENTS.into()),
    }
}

fn transform_args(args: &[String]) -> Result<(f64, f64, f64, Option<ClipArgs>), String> {
    let [x, y, zoom, rest @ ..] = args else {
        return Err(INVALID_ARGUMENTS.into());
    };
    let finite = |text: &str| {
        text.parse::<f64>()
            .ok()
            .filter(|value| value.is_finite())
            .ok_or(INVALID_ARGUMENTS)
    };
    let (x, y, zoom) = (finite(x)?, finite(y)?, finite(zoom)?);
    if !(0.1..=10.0).contains(&zoom) {
        return Err(INVALID_ARGUMENTS.into());
    }
    Ok((x, y, zoom, clip_args(rest)?))
}

#[derive(Debug, PartialEq)]
enum Edit {
    Begin,
    Update(f64, f64, f64, Option<ClipArgs>),
    Cancel,
    Apply,
}

#[derive(Debug, PartialEq)]
enum Request<'a> {
    Status,
    Catalog(Option<&'a str>),
    Panel(Option<&'a str>),
    Motions,
    Expressions,
    Outputs,
    FocusedOutput,
    Gpu(Option<&'a str>),
    SaveTransform,
    Skin,
    Output(Option<&'a str>),
    Opacity(f64),
    HoverOpacity(Option<f64>),
    Crop(Option<ClipArgs>),
    Transform(f64, f64, f64, Option<ClipArgs>),
    Edit(Edit),
    Load(&'a str),
    Show,
    Hide,
    Motion { group: &'a str, index: u32 },
    Expression(&'a str),
    ToggleExpression { session: u64, id: &'a str },
    LipSync(Option<f32>),
    Look(Option<(f32, f32)>),
}

impl<'a> Request<'a> {
    fn parse(args: &'a [String]) -> Result<Self, String> {
        let (name, args) = args.split_first().ok_or(INVALID_ARGUMENTS)?;
        let valid_text = |text: &str| !text.is_empty() && !text.contains('\0');
        let normalized = |text: &str, minimum| {
            let value = text.parse::<f32>().map_err(|_| INVALID_ARGUMENTS)?;
            (minimum..=1.0)
                .contains(&value)
                .then_some(value)
                .ok_or(INVALID_ARGUMENTS)
        };
        match (name.as_str(), args) {
            ("status", []) => Ok(Self::Status),
            ("catalog", []) => Ok(Self::Catalog(None)),
            ("panel", []) => Ok(Self::Panel(None)),
            ("panel", [directory]) if valid_text(directory) => Ok(Self::Panel(Some(directory))),
            ("catalog", [directory]) if valid_text(directory) => Ok(Self::Catalog(Some(directory))),
            ("motions", []) => Ok(Self::Motions),
            ("lip-sync", []) => Ok(Self::LipSync(None)),
            ("lip-sync", [opening]) => Ok(Self::LipSync(Some(normalized(opening, 0.0)?))),
            ("look", []) => Ok(Self::Look(None)),
            ("look", [x, y]) => Ok(Self::Look(Some((
                normalized(x, -1.0)?,
                normalized(y, -1.0)?,
            )))),
            ("expressions", []) => Ok(Self::Expressions),
            ("skin", []) => Ok(Self::Skin),
            ("save-transform", []) => Ok(Self::SaveTransform),
            ("gpu", []) => Ok(Self::Gpu(None)),
            ("gpu", [value]) if valid_gpu_selector(value) => Ok(Self::Gpu(Some(value))),
            ("focused-output", []) => Ok(Self::FocusedOutput),
            ("outputs", []) => Ok(Self::Outputs),
            ("output", [name]) if valid_text(name) => Ok(Self::Output(Some(name))),
            ("output", []) => Ok(Self::Output(None)),
            ("crop", args) => Ok(Self::Crop(clip_args(args)?)),
            ("transform", args) => {
                let (x, y, zoom, clip) = transform_args(args)?;
                Ok(Self::Transform(x, y, zoom, clip))
            }
            ("edit", [command, rest @ ..]) => match (command.as_str(), rest) {
                ("begin", []) => Ok(Self::Edit(Edit::Begin)),
                ("apply", []) => Ok(Self::Edit(Edit::Apply)),
                ("cancel", []) => Ok(Self::Edit(Edit::Cancel)),
                ("update", rest) => {
                    let (x, y, zoom, clip) = transform_args(rest)?;
                    Ok(Self::Edit(Edit::Update(x, y, zoom, clip)))
                }
                _ => Err(INVALID_ARGUMENTS.into()),
            },
            ("hover-opacity", []) => Ok(Self::HoverOpacity(None)),
            ("opacity" | "hover-opacity", [value]) => {
                let value = value.parse::<f64>().map_err(|_| INVALID_ARGUMENTS)?;
                if !(0.0..=1.0).contains(&value) {
                    return Err(INVALID_ARGUMENTS.into());
                }
                Ok(if name == "opacity" {
                    Self::Opacity(value)
                } else {
                    Self::HoverOpacity(Some(value))
                })
            }
            ("load", [path]) if valid_text(path) => Ok(Self::Load(path)),
            ("show", []) => Ok(Self::Show),
            ("hide", []) => Ok(Self::Hide),
            ("motion", [group, index]) if valid_text(group) => Ok(Self::Motion {
                group,
                index: index.parse().map_err(|_| INVALID_ARGUMENTS)?,
            }),
            ("toggle-expression", [session, id]) if valid_text(id) => Ok(Self::ToggleExpression {
                session: session.parse().map_err(|_| INVALID_ARGUMENTS)?,
                id,
            }),
            ("expression", [name]) if valid_text(name) => Ok(Self::Expression(name)),
            (
                "status" | "catalog" | "panel" | "load" | "show" | "hide" | "motion" | "motions"
                | "expressions" | "expression" | "outputs" | "output" | "gpu" | "focused-output"
                | "toggle-expression" | "opacity" | "hover-opacity" | "edit" | "lip-sync" | "look",
                _,
            ) => Err(INVALID_ARGUMENTS.into()),
            _ => Err("unknown_command".into()),
        }
    }
}

#[derive(Serialize)]
struct Response<T> {
    #[serde(skip_serializing_if = "Option::is_none")]
    accepted_operation_id: Option<u64>,
    snapshot: T,
}

fn response(
    accepted_operation_id: Option<u64>,
    snapshot: impl Serialize,
) -> Result<String, String> {
    serde_json::to_string(&Response {
        accepted_operation_id,
        snapshot,
    })
    .map_err(|error| format!("terminal status serialization failed: {error}"))
}

// Keep the snapshot shape consistent with the native backend when there is no worker.
fn unavailable_snapshot(error: &str) -> serde_json::Value {
    serde_json::json!({
        "state": "unavailable",
        "model": null,
        "session": 0,
        "visible": false,
        "opacity": 1.0,
        "hover_opacity": null,
        "mouse_look_radius": null,
        "crop": null,
        "edit_owner": null,
        "screen_editing": false,
        "screen_save": null,
        "output_save": null,
        "screen_save_error": null,
        "transform": {"x": 0.0, "y": 0.0, "zoom": 1.0, "crop": null},
        "output": null,
        "selected_output": null,
        "outputs": [],
        "active_gpu": null,
        "gpu_devices": [],
        "error": error,
        "pending": [],
        "completed": [],
        "motions": [],
        "expressions": [],
    })
}

fn valid_gpu_selector(value: &str) -> bool {
    if value == "auto" {
        return true;
    }
    if let Some(node) = value.strip_prefix("/dev/dri/renderD") {
        return node.parse::<u32>().is_ok() && node.bytes().all(|byte| byte.is_ascii_digit());
    }
    value.len() == 12
        && value.bytes().enumerate().all(|(index, byte)| match index {
            4 | 7 => byte == b':',
            10 => byte == b'.',
            11 => (b'0'..=b'7').contains(&byte),
            _ => byte.is_ascii_hexdigit(),
        })
}

struct TerminalPlugin {
    model_directory: Option<String>,
    gpu: String,
    #[cfg(all(target_os = "linux", feature = "native"))]
    startup_gpu: String,
    #[cfg(all(target_os = "linux", feature = "native"))]
    startup_model: Option<String>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    startup_output: Option<String>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    last_wheel_save: Option<std::time::Instant>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    last_screen_save: Option<u64>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    last_output_save: Option<u64>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    wheel_save_error: Option<String>,
    opacity: f64,
    hover_opacity: Option<f64>,
    mouse_look_radius: Option<f64>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    animate: bool,
    #[cfg(all(target_os = "linux", feature = "native"))]
    expression_timeout: Option<f32>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    transform: Transform,
    #[cfg(all(target_os = "linux", feature = "native"))]
    registered_owner: Option<ConnectionId>,
    #[cfg(all(target_os = "linux", feature = "native"))]
    worker: Option<Result<Worker, String>>,
}

impl Default for TerminalPlugin {
    fn default() -> Self {
        Self {
            model_directory: None,
            gpu: "auto".to_owned(),
            #[cfg(all(target_os = "linux", feature = "native"))]
            startup_gpu: "auto".to_owned(),
            #[cfg(all(target_os = "linux", feature = "native"))]
            startup_model: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            startup_output: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            last_wheel_save: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            last_screen_save: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            last_output_save: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            wheel_save_error: None,
            opacity: 1.0,
            hover_opacity: None,
            mouse_look_radius: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            animate: true,
            #[cfg(all(target_os = "linux", feature = "native"))]
            expression_timeout: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            transform: Transform::default(),
            #[cfg(all(target_os = "linux", feature = "native"))]
            registered_owner: None,
            #[cfg(all(target_os = "linux", feature = "native"))]
            worker: None,
        }
    }
}

impl TerminalPlugin {
    fn dispatch(&mut self, request: Request<'_>) -> Result<String, String> {
        match request {
            Request::Catalog(directory) => {
                catalog::response(directory, self.model_directory.as_deref())
            }
            Request::Panel(directory) => {
                let catalog = catalog::response(directory, self.model_directory.as_deref());
                let status = self.dispatch_overlay(Request::Status)?;
                panel::describe(catalog, &status)
            }
            _ => self.dispatch_overlay(request),
        }
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn dispatch_overlay(&mut self, request: Request<'_>) -> Result<String, String> {
        let worker = match self.worker.get_or_insert_with(|| {
            let configured_root = self.model_directory.clone();
            Worker::start(
                layer_shell_probe::WorkerSettings {
                    opacity: self.opacity,
                    hover_opacity: self.hover_opacity,
                    transform: self.transform,
                    mouse_look_radius: self.mouse_look_radius,
                    requested_gpu: self.startup_gpu.clone(),
                    animate: self.animate,
                    expression_timeout: self.expression_timeout,
                    startup_output: self.startup_output.clone(),
                },
                move || {
                    catalog::models(configured_root.as_deref())
                        .map(|models| models.into_iter().map(Into::into).collect())
                },
                (
                    || compositor::focused_output().map_err(|error| error.to_string()),
                    || match compositor::hyprland_pointer() {
                        Ok(sample) => Ok(Some(layer_shell_probe::DragOutput {
                            connector: sample.output,
                            pointer: (f64::from(sample.position.0), f64::from(sample.position.1)),
                            origin: (
                                f64::from(sample.output_origin.0),
                                f64::from(sample.output_origin.1),
                            ),
                        })),
                        Err(compositor::FocusedOutputError::Unsupported) => Ok(None),
                        Err(error) => Err(error.to_string()),
                    },
                    global_gaze(),
                ),
            )
        }) {
            Ok(worker) => worker,
            Err(error)
                if matches!(
                    request,
                    Request::Status | Request::Motions | Request::Expressions | Request::Outputs
                ) =>
            {
                return response(None, unavailable_snapshot(error));
            }
            Err(error) => return Err(error.clone()),
        };
        let command = match request {
            Request::Status | Request::Motions | Request::Expressions | Request::Outputs => {
                let mut snapshot =
                    serde_json::to_value(worker.status()).map_err(|error| error.to_string())?;
                snapshot["configured_gpu"] = serde_json::json!(self.gpu);
                snapshot["restart_required"] = serde_json::json!(self.gpu != self.startup_gpu);
                snapshot["transform_save_error"] = serde_json::json!(self.wheel_save_error);
                return response(None, snapshot);
            }
            Request::Catalog(_) | Request::Panel(_) => unreachable!("handled separately"),
            Request::Load(path) => Command::Load(path.into()),
            Request::Show => Command::Show,
            Request::Hide => Command::Hide,
            Request::Motion { group, index } => Command::Motion {
                group: group.into(),
                index,
            },
            Request::Expression(name) => Command::Expression(name.into()),
            Request::ToggleExpression { session, id } => {
                let status = worker.status();
                if status.session != session || status.model.is_none() {
                    return Err("model session changed before the shortcut could run".into());
                }
                if status.edit_owner.is_some() || status.screen_editing {
                    return Err("finish the active transform edit first".into());
                }
                let path = status.model.expect("checked loaded model");
                let binding = vtube::load(&path)?
                    .into_iter()
                    .find(|binding| binding.id == id)
                    .ok_or("unknown model shortcut")?;
                if let Some(reason) = binding.unsupported_reason {
                    return Err(reason);
                }
                if !binding.enabled {
                    return Err("model shortcut is disabled".into());
                }
                Command::ToggleExpression {
                    session,
                    file: binding.expression.ok_or("shortcut expression is missing")?,
                    fade_seconds: binding.fade.as_secs_f32(),
                    deactivate_after: binding.duration.map(|duration| duration.as_secs_f32()),
                }
            }
            Request::LipSync(opening) => Command::LipSync(opening),
            Request::Look(target) => Command::Look(target),
            Request::Skin => Command::Skin,
            Request::FocusedOutput => Command::FocusedOutput,
            Request::Output(name) => Command::Output(name.map(str::to_owned)),
            Request::Opacity(value) => Command::Opacity(value),
            Request::HoverOpacity(value) => Command::HoverOpacity(value),
            Request::Crop(rect) => Command::Crop(
                rect.and_then(|(x, y, w, h)| layer_shell_probe::Crop::new(x, y, w, h)),
            ),
            Request::Transform(x, y, zoom, rect) => {
                Command::Transform(checked_transform(x, y, zoom, rect)?)
            }
            Request::Edit(_) | Request::SaveTransform | Request::Gpu(_) => {
                unreachable!("handled separately")
            }
        };
        let id = worker.submit(command)?;
        response(Some(id), worker.status())
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn edit(&mut self, edit: Edit, cx: &mut Context<'_>) -> Result<String, String> {
        let owner = cx
            .connection_id()
            .ok_or("transform editing requires a persistent connection")?;
        let worker = self
            .worker
            .as_mut()
            .and_then(|worker| worker.as_mut().ok())
            .ok_or("no model is loaded")?;
        let status = worker.status();
        if status.state != "ready" {
            return Err("terminal worker is not ready".into());
        }
        let command = match edit {
            Edit::Begin if !status.screen_editing && status.edit_owner.is_none() && status.pending.is_empty() && status.model.is_some() => {
                Command::BeginTransform { owner: owner.0 }
            }
            Edit::Begin => return Err("cannot start an edit while no model is loaded, another edit is active, or commands are pending".into()),
            _ if status.edit_owner != Some(owner.0) => return Err("transform edit is not owned by this connection".into()),
            Edit::Update(x, y, zoom, clip) => Command::UpdateTransform {
                owner: owner.0,
                transform: checked_transform(x, y, zoom, clip)?,
            },
            Edit::Cancel => Command::EndTransform { owner: owner.0, apply: false },
            Edit::Apply => {
                if !status.pending.is_empty() {
                    return Err("wait for the transform preview to finish before saving".into());
                }
                persist_transform(cx, status.transform)?;
                self.transform = status.transform;
                Command::EndTransform { owner: owner.0, apply: true }
            }
        };
        let id = worker.submit(command)?;
        if matches!(edit, Edit::Begin) {
            self.registered_owner = Some(owner);
        }
        response(Some(id), worker.status())
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn save_screen_transform(&mut self, cx: &mut Context<'_>) {
        let Some(Ok(worker)) = self.worker.as_mut() else {
            return;
        };
        let status = worker.status();
        let Some(save) = status
            .screen_save
            .filter(|save| Some(save.id) != self.last_screen_save)
        else {
            return;
        };
        let result = persist_transform(cx, save.transform);
        self.last_screen_save = Some(save.id);
        if result.is_ok() {
            self.transform = save.transform;
        }
        if let Err(error) = &result {
            eprintln!("terminal screen edit save: {error}");
        }
        worker.finish_screen_save(save.id, result.err());
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn save_output(&mut self, cx: &mut Context<'_>) {
        let Some(Ok(worker)) = self.worker.as_mut() else {
            return;
        };
        let Some(save) = worker
            .status()
            .output_save
            .filter(|save| Some(save.id) != self.last_output_save)
        else {
            return;
        };
        let result = (|| {
            let mut section = cx.config().unwrap_or_else(|| serde_json::json!({}));
            section["transform"] =
                serde_json::to_value(save.transform).map_err(|error| error.to_string())?;
            section["output"] = serde_json::json!(save.output);
            cx.replace_config(section)
        })();
        self.last_output_save = Some(save.id);
        if result.is_ok() {
            self.transform = save.transform;
            self.startup_output = save.output;
        }
        worker.finish_output_save(save.id, result.err());
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn save_wheel_transform(&mut self, cx: &mut Context<'_>, retry: bool) -> Result<(), String> {
        let Some(status) = self
            .worker
            .as_ref()
            .and_then(|worker| worker.as_ref().ok())
            .map(Worker::status)
        else {
            return if retry {
                Err("no overlay is running".into())
            } else {
                Ok(())
            };
        };
        let Some((changed, transform)) = status
            .wheel_change
            .filter(|(_, transform)| *transform == status.transform)
        else {
            self.wheel_save_error = None;
            return if retry {
                Err("no wheel transform to save".into())
            } else {
                Ok(())
            };
        };
        if status.state != "ready"
            || status.screen_editing
            || status.edit_owner.is_some()
            || !status.pending.is_empty()
        {
            return if retry {
                Err("wait for the overlay to finish editing".into())
            } else {
                Ok(())
            };
        }
        if !retry
            && (self.last_wheel_save == Some(changed)
                || changed.elapsed() < std::time::Duration::from_millis(500))
        {
            return Ok(());
        }
        let result = persist_transform(cx, transform);
        self.last_wheel_save = Some(changed);
        self.wheel_save_error = result.as_ref().err().cloned();
        if result.is_ok() {
            self.transform = transform;
        }
        result
    }

    #[cfg(not(all(target_os = "linux", feature = "native")))]
    fn dispatch_overlay(&mut self, request: Request<'_>) -> Result<String, String> {
        match request {
            Request::Status | Request::Motions | Request::Expressions | Request::Outputs => {
                response(None, unavailable_snapshot(UNAVAILABLE))
            }
            _ => Err(UNAVAILABLE.into()),
        }
    }
}

#[cfg(all(target_os = "linux", feature = "native"))]
fn persist_transform(cx: &mut Context<'_>, transform: Transform) -> Result<(), String> {
    let mut section = cx.config().unwrap_or_else(|| serde_json::json!({}));
    section["transform"] = serde_json::to_value(transform).map_err(|error| error.to_string())?;
    cx.replace_config(section)
}

#[cfg(all(target_os = "linux", feature = "native"))]
fn checked_transform(
    x: f64,
    y: f64,
    zoom: f64,
    clip: Option<ClipArgs>,
) -> Result<Transform, String> {
    let clip = clip
        .map(|(x, y, w, h)| Crop::new(x, y, w, h).ok_or(INVALID_ARGUMENTS))
        .transpose()?;
    Transform::new(x, y, zoom, clip).ok_or_else(|| INVALID_ARGUMENTS.into())
}

#[cfg(all(target_os = "linux", feature = "native"))]
fn config_transform(value: &serde_json::Value) -> Option<Transform> {
    let crop = match &value["crop"] {
        serde_json::Value::Null => None,
        crop => Some(Crop::new(
            crop["x"].as_i64()?.try_into().ok()?,
            crop["y"].as_i64()?.try_into().ok()?,
            crop["width"].as_u64()?.try_into().ok()?,
            crop["height"].as_u64()?.try_into().ok()?,
        )?),
    };
    Transform::new(
        value["x"].as_f64()?,
        value["y"].as_f64()?,
        value["zoom"].as_f64()?,
        crop,
    )
}

deskwaifu_sdk::export_plugin!(TerminalPlugin::default());

impl Plugin for TerminalPlugin {
    fn plugin_id(&self) -> &str {
        "terminal"
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        if let Some(config) = cx.config() {
            let config = config
                .as_object()
                .ok_or("terminal configuration must be an object")?;
            #[cfg(all(target_os = "linux", feature = "native"))]
            if let Some(transform) = config.get("transform") {
                self.transform = config_transform(transform).ok_or("invalid transform")?;
            }
            if let Some(opacity) = config.get("opacity") {
                self.opacity = opacity
                    .as_f64()
                    .filter(|value| (0.0..=1.0).contains(value))
                    .ok_or("invalid opacity")?;
            }
            if let Some(hover_opacity) =
                config.get("hover_opacity").filter(|value| !value.is_null())
            {
                self.hover_opacity = Some(
                    hover_opacity
                        .as_f64()
                        .filter(|value| (0.0..=1.0).contains(value))
                        .ok_or("invalid hover_opacity")?,
                );
            }
            if let Some(radius) = config
                .get("mouse_look_radius")
                .filter(|value| !value.is_null())
            {
                self.mouse_look_radius = Some(
                    radius
                        .as_f64()
                        .filter(|value| value.is_finite() && *value >= 0.0)
                        .ok_or("invalid mouse_look_radius")?,
                );
            }
            #[cfg(all(target_os = "linux", feature = "native"))]
            if let Some(path) = config.get("startup_model").filter(|value| !value.is_null()) {
                self.startup_model = Some(
                    path.as_str()
                        .filter(|path| {
                            std::path::Path::new(path).is_absolute() && !path.contains('\0')
                        })
                        .ok_or("startup_model must be an absolute path")?
                        .to_owned(),
                );
            }
            if let Some(gpu) = config.get("gpu") {
                self.gpu = gpu
                    .as_str()
                    .filter(|value| valid_gpu_selector(value))
                    .ok_or("invalid terminal GPU selector")?
                    .to_owned();
            }
            #[cfg(all(target_os = "linux", feature = "native"))]
            if let Some(animate) = config.get("animate") {
                self.animate = animate
                    .as_bool()
                    .ok_or("terminal animate must be a boolean")?;
            }
            #[cfg(all(target_os = "linux", feature = "native"))]
            if let Some(output) = config.get("output").filter(|value| !value.is_null()) {
                self.startup_output = Some(
                    output
                        .as_str()
                        .filter(|name| {
                            !name.is_empty()
                                && name.len() <= 256
                                && !name.chars().any(char::is_control)
                        })
                        .ok_or("invalid terminal output name")?
                        .to_owned(),
                );
            }
            #[cfg(all(target_os = "linux", feature = "native"))]
            if let Some(value) = config.get("emotion_timeout") {
                let seconds = value
                    .as_f64()
                    .filter(|v| *v == -1.0 || (0.0..=86_400.0).contains(v))
                    .ok_or("invalid emotion_timeout")?;
                self.expression_timeout = (seconds >= 0.0).then_some(seconds as f32);
            }
            if let Some(directory) = config.get("model_directory") {
                self.model_directory = Some(
                    directory
                        .as_str()
                        .filter(|path| !path.is_empty() && !path.contains('\0'))
                        .ok_or("invalid model_directory")?
                        .to_owned(),
                );
            }
        }
        #[cfg(all(target_os = "linux", feature = "native"))]
        self.startup_gpu.clone_from(&self.gpu);
        cx.register_command(
            "terminal",
            vec![
                "Control the terminal overlay.".into(),
                "terminal status              Read the latest status snapshot.".into(),
                "terminal catalog [DIRECTORY] Discover model files without loading them.".into(),
                "terminal load PATH           Queue a model load.".into(),
                "terminal show                Queue showing the overlay.".into(),
                "terminal hide                Queue hiding the overlay.".into(),
                "terminal motion GROUP INDEX  Queue a motion by group and zero-based index.".into(),
                "terminal motions             Read motion groups from the status snapshot.".into(),
                "terminal expressions         Read expression names from the status snapshot."
                    .into(),
                "terminal expression NAME     Queue an expression by name.".into(),
                "terminal hover-opacity [VALUE] Preview hover opacity or clear it.".into(),
                "terminal opacity VALUE       Preview opacity between 0 and 1.".into(),
                "terminal transform X Y Z [CX CY CW CH] Preview an atomic transform.".into(),
                "terminal crop [X Y W H]     Preview a clip or reset it.".into(),
                "terminal lip-sync [VALUE]    Override mouth opening or restore animation.".into(),
                "terminal look [X Y]          Override normalized gaze or restore animation."
                    .into(),
                "terminal skin                Cycle to the next model skin.".into(),
                "terminal save-transform      Retry saving a wheel transform.".into(),
                "terminal gpu [auto|RENDER_NODE|PCI] Set the EGL device for the next host start."
                    .into(),
                "terminal focused-output      Move to the currently focused output.".into(),
                "terminal outputs             List outputs in the status snapshot.".into(),
                "terminal output [NAME]       Select an output, or allow compositor selection."
                    .into(),
            ],
        )?;
        cx.register_panel_tab(PanelTab {
            id: "models".into(),
            title: "模型".into(),
            order: 10,
            query: Some(PanelQuery {
                args: vec!["terminal".into(), "panel".into()],
                input_placeholder: Some("后端默认模型目录".into()),
            }),
            sections: vec![PanelSection {
                title: "模型显示".into(),
                actions: [("显示", "show"), ("隐藏", "hide")]
                    .into_iter()
                    .map(|(label, command)| PanelAction {
                        label: label.into(),
                        args: vec!["terminal".into(), command.into()],
                    })
                    .collect(),
            }],
        })
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn start(&mut self, _: &mut Context<'_>) -> PluginResult {
        if let Some(path) = self.startup_model.take() {
            if let Err(error) = self.dispatch_overlay(Request::Load(&path)) {
                eprintln!("terminal startup model: {error}");
            }
        }
        Ok(())
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn tick_interval(&self) -> Option<std::time::Duration> {
        Some(std::time::Duration::from_millis(250))
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn tick(&mut self, cx: &mut Context<'_>) -> PluginResult {
        self.save_output(cx);
        self.save_screen_transform(cx);
        if let Err(error) = self.save_wheel_transform(cx, false) {
            eprintln!("terminal transform autosave: {error}");
        }
        Ok(())
    }

    fn on_command(
        &mut self,
        name: &str,
        args: &[String],
        cx: &mut Context<'_>,
    ) -> Result<String, String> {
        if name != "terminal" {
            return Err(format!("unhandled command: {name}"));
        }
        let request = Request::parse(args)?;
        #[cfg(not(all(target_os = "linux", feature = "native")))]
        if matches!(request, Request::Gpu(_)) {
            return Err(UNAVAILABLE.into());
        }
        #[cfg(all(target_os = "linux", feature = "native"))]
        if let Request::Gpu(selector) = request {
            if let Some(selector) = selector {
                let mut section = cx.config().unwrap_or_else(|| serde_json::json!({}));
                section["gpu"] = serde_json::json!(selector);
                cx.replace_config(section)?;
                self.gpu = selector.to_owned();
            }
            return self.dispatch_overlay(Request::Status);
        }
        #[cfg(all(target_os = "linux", feature = "native"))]
        if matches!(request, Request::SaveTransform) {
            self.save_wheel_transform(cx, true)?;
            return self.dispatch_overlay(Request::Status);
        }
        #[cfg(all(target_os = "linux", feature = "native"))]
        if let Request::Edit(edit) = request {
            return self.edit(edit, cx);
        }
        #[cfg(not(all(target_os = "linux", feature = "native")))]
        let _ = cx;
        self.dispatch(request)
    }

    fn on_message(&mut self, _message: &Message, _cx: &mut Context<'_>) -> PluginResult {
        Ok(())
    }

    #[cfg(all(target_os = "linux", feature = "native"))]
    fn on_disconnect(&mut self, connection: ConnectionId, _: &mut Context<'_>) {
        if self.registered_owner == Some(connection) {
            self.registered_owner = None;
            if let Some(Ok(worker)) = &mut self.worker {
                worker.cancel_disconnected_edit(connection.0);
            }
        }
    }

    fn stop(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        // Dropping the worker signals shutdown independently of the command queue and joins it.
        #[cfg(all(target_os = "linux", feature = "native"))]
        drop(self.worker.take());
        Ok(())
    }
}

#[cfg(test)]
mod tests;
