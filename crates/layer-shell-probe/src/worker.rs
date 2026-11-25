use serde::Serialize;
use std::{
    collections::VecDeque,
    io::{self, Read, Write},
    os::unix::net::UnixStream,
    path::PathBuf,
    sync::{
        Arc, Mutex,
        mpsc::{self, Receiver, SyncSender, TrySendError},
    },
    thread::{self, JoinHandle},
};

const CAPACITY: usize = 16;
const HISTORY: usize = 32;

pub enum Command {
    Load(PathBuf),
    Show,
    Hide,
    Motion {
        group: String,
        index: u32,
    },
    Expression(String),
    ToggleExpression {
        session: u64,
        file: PathBuf,
        fade_seconds: f32,
        deactivate_after: Option<f32>,
    },
    LipSync(Option<f32>),
    Look(Option<(f32, f32)>),
    Output(Option<String>),
    FocusedOutput,
    Skin,
    Opacity(f64),
    HoverOpacity(Option<f64>),
    Crop(Option<crate::Crop>),
    Transform(crate::Transform),
    BeginTransform {
        owner: u64,
    },
    UpdateTransform {
        owner: u64,
        transform: crate::Transform,
    },
    EndTransform {
        owner: u64,
        apply: bool,
    },
}

pub(crate) struct PendingCommand {
    pub id: u64,
    pub command: Command,
    session: Option<u64>,
}

impl PendingCommand {
    fn new(id: u64, command: Command, session: u64) -> Self {
        let session = match &command {
            Command::ToggleExpression { session, .. } => Some(*session),
            Command::Motion { .. }
            | Command::Expression(_)
            | Command::LipSync(_)
            | Command::Look(_)
            | Command::Skin => Some(session),
            _ => None,
        };
        Self {
            id,
            command,
            session,
        }
    }

    pub(crate) fn is_current(&self, session: u64) -> bool {
        self.session.is_none_or(|expected| expected == session)
    }
}

#[derive(Clone, Serialize)]
pub struct MotionGroup {
    pub name: String,
    pub count: u32,
}

#[derive(Clone, Copy, Serialize)]
pub struct ScreenSave {
    pub id: u64,
    pub transform: crate::Transform,
}

type SaveResult = Option<(u64, Option<String>)>;

#[derive(Clone, Serialize)]
pub struct OutputSave {
    pub id: u64,
    pub output: Option<String>,
    pub transform: crate::Transform,
}

#[derive(Clone, Serialize)]
pub struct GpuDevice {
    pub render_node: Option<PathBuf>,
    pub pci: Option<String>,
}

impl From<cubism_bridge::RenderDevice> for GpuDevice {
    fn from(device: cubism_bridge::RenderDevice) -> Self {
        Self {
            render_node: device.render_node,
            pci: device.pci,
        }
    }
}

#[derive(Clone, Serialize)]
pub struct Status {
    pub active_gpu: Option<GpuDevice>,
    pub gpu_devices: Vec<GpuDevice>,
    pub requested_gpu: String,
    pub state: &'static str,
    pub model: Option<PathBuf>,
    pub session: u64,
    pub visible: bool,
    pub opacity: f64,
    pub hover_opacity: Option<f64>,
    pub mouse_look_radius: Option<f64>,
    pub crop: Option<crate::Crop>,
    pub transform: crate::Transform,
    #[serde(skip)]
    pub wheel_change: Option<(std::time::Instant, crate::Transform)>,
    pub edit_owner: Option<u64>,
    pub screen_editing: bool,
    pub screen_save: Option<ScreenSave>,
    pub screen_save_error: Option<String>,
    pub output_save: Option<OutputSave>,
    pub output: Option<String>,
    pub selected_output: Option<String>,
    pub outputs: Vec<String>,
    pub error: Option<String>,
    pub pending: Vec<u64>,
    pub motions: Vec<MotionGroup>,
    pub expressions: Vec<String>,
    pub active_expression_toggles: Vec<PathBuf>,
    pub completed: VecDeque<Operation>,
}

#[derive(Clone, Serialize)]
pub struct Operation {
    pub id: u64,
    pub error: Option<String>,
}

pub struct Worker {
    tx: SyncSender<PendingCommand>,
    wake: UnixStream,
    status: Arc<Mutex<Status>>,
    canceled_edit: Arc<Mutex<Option<u64>>>,
    screen_save_result: Arc<Mutex<SaveResult>>,
    output_save_result: Arc<Mutex<SaveResult>>,
    next: u64,
    thread: Option<JoinHandle<()>>,
}

/// A sampled compositor-global pointer and the connector containing it.
/// Both coordinates are in logical pixels in the compositor's global layout.
#[derive(Debug)]
pub struct DragOutput {
    pub connector: String,
    pub pointer: (f64, f64),
    pub origin: (f64, f64),
}

/// Sample an output-local gaze target in logical pixels. A missing sample falls back to Wayland input.
pub type GlobalGaze = Box<dyn FnMut(&str, (u32, u32)) -> Option<(f64, f64)> + Send>;

pub(crate) struct Control {
    pub(crate) requested_gpu: String,
    pub(crate) startup_output: Option<String>,
    pub(crate) animate: bool,
    pub(crate) expression_timeout: Option<f32>,
    pub(crate) focused_output: Box<dyn Fn() -> Result<String, String> + Send>,
    pub(crate) global_gaze: Option<GlobalGaze>,
    pub(crate) drag_output: Box<dyn Fn() -> Result<Option<DragOutput>, String> + Send>,
    pub(crate) catalog: Box<dyn Fn() -> Result<Vec<PathBuf>, String> + Send>,
    rx: Receiver<PendingCommand>,
    pub(crate) wake: UnixStream,
    status: Arc<Mutex<Status>>,
    canceled_edit: Arc<Mutex<Option<u64>>>,
    screen_save_result: Arc<Mutex<SaveResult>>,
    output_save_result: Arc<Mutex<SaveResult>>,
}

pub struct WorkerSettings {
    pub opacity: f64,
    pub hover_opacity: Option<f64>,
    pub transform: crate::Transform,
    pub mouse_look_radius: Option<f64>,
    pub requested_gpu: String,
    pub startup_output: Option<String>,
    pub animate: bool,
    pub expression_timeout: Option<f32>,
}

impl Worker {
    pub fn start(
        settings: WorkerSettings,
        catalog: impl Fn() -> Result<Vec<PathBuf>, String> + Send + 'static,
        (focused_output, drag_output, global_gaze): (
            impl Fn() -> Result<String, String> + Send + 'static,
            impl Fn() -> Result<Option<DragOutput>, String> + Send + 'static,
            Option<GlobalGaze>,
        ),
    ) -> Result<Self, String> {
        let WorkerSettings {
            opacity,
            hover_opacity,
            transform,
            mouse_look_radius,
            requested_gpu,
            animate,
            expression_timeout,
            startup_output,
        } = settings;
        if !(0.0..=1.0).contains(&opacity)
            || hover_opacity.is_some_and(|value| !(0.0..=1.0).contains(&value))
        {
            return Err("opacity must be between 0 and 1".into());
        }
        if mouse_look_radius.is_some_and(|radius| !radius.is_finite() || radius < 0.0) {
            return Err("mouse look radius must be finite and non-negative".into());
        }
        if expression_timeout.is_some_and(|seconds| !seconds.is_finite() || seconds < 0.0) {
            return Err("expression timeout must be finite and non-negative".into());
        }
        let (tx, rx) = mpsc::sync_channel(CAPACITY);
        let (wake, reader) = UnixStream::pair().map_err(|error| error.to_string())?;
        wake.set_nonblocking(true)
            .map_err(|error| error.to_string())?;
        reader
            .set_nonblocking(true)
            .map_err(|error| error.to_string())?;
        let status = Arc::new(Mutex::new(Status {
            active_gpu: None,
            gpu_devices: Vec::new(),
            requested_gpu: requested_gpu.clone(),
            state: "starting",
            model: None,
            session: 0,
            visible: false,
            opacity,
            hover_opacity,
            mouse_look_radius,
            crop: transform.crop,
            transform,
            wheel_change: None,
            edit_owner: None,
            screen_editing: false,
            screen_save: None,
            screen_save_error: None,
            output_save: None,
            output: None,
            selected_output: startup_output.clone(),
            outputs: Vec::new(),
            error: None,
            pending: Vec::new(),
            motions: Vec::new(),
            expressions: Vec::new(),
            active_expression_toggles: Vec::new(),
            completed: VecDeque::new(),
        }));
        let canceled_edit = Arc::new(Mutex::new(None));
        let screen_save_result = Arc::new(Mutex::new(None));
        let output_save_result = Arc::new(Mutex::new(None));
        let mut control = Control {
            requested_gpu,
            startup_output,
            animate,
            expression_timeout,
            catalog: Box::new(catalog),
            focused_output: Box::new(focused_output),
            drag_output: Box::new(drag_output),
            global_gaze,
            rx,
            wake: reader,
            status: Arc::clone(&status),
            canceled_edit: Arc::clone(&canceled_edit),
            screen_save_result: Arc::clone(&screen_save_result),
            output_save_result: Arc::clone(&output_save_result),
        };
        let worker_status = Arc::clone(&status);
        let thread = thread::Builder::new()
            .name("deskwaifu-terminal".into())
            .spawn(move || {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    crate::probe::run_managed(&mut control).map_err(|error| error.to_string())
                }));
                let mut snapshot = worker_status.lock().unwrap_or_else(|p| p.into_inner());
                snapshot.active_gpu = None;
                snapshot.visible = false;
                snapshot.active_expression_toggles.clear();
                snapshot.edit_owner = None;
                snapshot.screen_editing = false;
                snapshot.screen_save = None;
                snapshot.output_save = None;
                snapshot.output = None;
                snapshot.outputs.clear();
                snapshot.error = match result {
                    Ok(result) => result.err(),
                    Err(_) => Some("terminal worker panicked".into()),
                };
                snapshot.state = if snapshot.error.is_some() {
                    "failed"
                } else {
                    "stopped"
                };
                for id in std::mem::take(&mut snapshot.pending) {
                    let error = snapshot
                        .error
                        .clone()
                        .unwrap_or_else(|| "terminal worker stopped".into());
                    snapshot.complete(id, Some(error));
                }
            })
            .map_err(|error| error.to_string())?;
        Ok(Self {
            tx,
            wake,
            status,
            canceled_edit,
            screen_save_result,
            output_save_result,
            next: 1,
            thread: Some(thread),
        })
    }

    pub fn status(&self) -> Status {
        self.status
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .clone()
    }

    pub fn submit(&mut self, command: Command) -> Result<u64, String> {
        let mut status = self.status.lock().unwrap_or_else(|p| p.into_inner());
        if let Some(error) = &status.error {
            return Err(error.clone());
        }
        let id = self.next;
        let next = id
            .checked_add(1)
            .ok_or("terminal operation IDs exhausted")?;
        self.tx
            .try_send(PendingCommand::new(id, command, status.session))
            .map_err(|error| match error {
                TrySendError::Full(_) => "terminal queue is full".to_owned(),
                TrySendError::Disconnected(_) => "terminal worker has stopped".to_owned(),
            })?;
        status.pending.push(id);
        self.next = next;
        drop(status);
        self.notify();
        Ok(id)
    }

    /// Disconnect rollback is independent of capacity in the business queue.
    pub fn cancel_disconnected_edit(&mut self, owner: u64) {
        *self.canceled_edit.lock().unwrap_or_else(|p| p.into_inner()) = Some(owner);
        self.notify();
    }

    /// A save acknowledgment must still reach the compositor when the command queue is full.
    pub fn finish_screen_save(&mut self, id: u64, error: Option<String>) {
        *self
            .screen_save_result
            .lock()
            .unwrap_or_else(|p| p.into_inner()) = Some((id, error));
        self.notify();
    }

    pub fn finish_output_save(&mut self, id: u64, error: Option<String>) {
        if self
            .status
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .output_save
            .as_ref()
            .is_none_or(|save| save.id != id)
        {
            return;
        }
        *self
            .output_save_result
            .lock()
            .unwrap_or_else(|p| p.into_inner()) = Some((id, error));
        self.notify();
    }

    fn notify(&mut self) {
        loop {
            match self.wake.write(&[1]) {
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                _ => break,
            }
        }
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        // EOF wakes the worker even when the command queue and socket buffer are full.
        let _ = self.wake.shutdown(std::net::Shutdown::Write);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

impl Status {
    pub(crate) fn complete(&mut self, id: u64, error: Option<String>) {
        self.pending.retain(|pending| *pending != id);
        if self.completed.len() == HISTORY {
            self.completed.pop_front();
        }
        self.completed.push_back(Operation { id, error });
    }
}

impl Control {
    pub(crate) fn open(&mut self) -> io::Result<bool> {
        let mut bytes = [0; 256];
        loop {
            match self.wake.read(&mut bytes) {
                Ok(0) => return Ok(false),
                Ok(_) => {}
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(true),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(error) => return Err(error),
            }
        }
    }

    pub(crate) fn next(&self) -> Option<PendingCommand> {
        self.rx.try_recv().ok()
    }

    pub(crate) fn take_canceled_edit(&self) -> Option<u64> {
        self.canceled_edit
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .take()
    }

    pub(crate) fn take_screen_save_result(&self) -> SaveResult {
        self.screen_save_result
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .take()
    }

    pub(crate) fn take_output_save_result(&self) -> SaveResult {
        self.output_save_result
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .take()
    }

    pub(crate) fn session(&self) -> u64 {
        self.status
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .session
    }

    pub(crate) fn model_path(&self) -> Option<PathBuf> {
        self.status
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .model
            .clone()
    }

    pub(crate) fn status_has_no_model(&self) -> bool {
        self.status
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .model
            .is_none()
    }

    pub(crate) fn update(&self, update: impl FnOnce(&mut Status)) {
        update(&mut self.status.lock().unwrap_or_else(|p| p.into_inner()));
    }

    pub(crate) fn complete(&self, id: u64, error: Option<String>) {
        self.update(|status| status.complete(id, error));
    }
}
