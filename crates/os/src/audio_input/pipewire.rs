use super::Recording;
use deskwaifu_sdk::OsError;
use pipewire as pw;
use pw::{
    properties::properties,
    spa,
    stream::{StreamFlags, StreamState},
};
use std::{
    cell::{Cell, RefCell},
    io::Cursor,
    rc::Rc,
    time::{Duration, Instant},
};

const START_TIMEOUT: Duration = Duration::from_secs(3);
const POLL_INTERVAL: Duration = Duration::from_millis(20);
const RATE: u32 = 16_000;
const MAX_CHUNK: usize = 32_000;

pub(super) fn record(recording: Rc<RefCell<Recording>>) -> Result<(), OsError> {
    if recording.borrow().cancelled() {
        return Err(OsError::Unavailable);
    }
    let startup_deadline = Instant::now() + START_TIMEOUT;
    let main_loop = pw::main_loop::MainLoopRc::new(None).map_err(|_| OsError::Unavailable)?;
    let context =
        pw::context::ContextRc::new(&main_loop, None).map_err(|_| OsError::Unavailable)?;
    let core = context.connect_rc(None).map_err(|_| OsError::Unavailable)?;
    let stream = pw::stream::StreamBox::new(
        &core,
        "DeskWaifu microphone",
        properties! {
            *pw::keys::MEDIA_TYPE => "Audio",
            *pw::keys::MEDIA_CATEGORY => "Capture",
            *pw::keys::MEDIA_ROLE => "Communication",
            // 固定本次采集的来源。断开时终止，避免悄悄切换到另一支麦克风。
            "node.dont-reconnect" => "true",
        },
    )
    .map_err(|_| OsError::Backend)?;

    let stopping = recording.borrow().stopping.clone();
    let chunks = recording.borrow().chunks.clone();
    let deadline = Rc::new(Cell::new(None));
    let error = Rc::new(Cell::new(None));
    let disconnected = Rc::new(Cell::new(false));
    let core_disconnected = disconnected.clone();
    let state_deadline = deadline.clone();
    let state_error = error.clone();
    let data_deadline = deadline.clone();
    let data_error = error.clone();
    let _core_listener = core
        .add_listener_local()
        .error(move |_, _, _, _| core_disconnected.set(true))
        .register();
    let _listener = stream
        .add_local_listener_with_user_data(recording)
        .state_changed(move |_, recording, old, new| match new {
            StreamState::Streaming if state_deadline.get().is_none() => {
                let mut recording = recording.borrow_mut();
                state_deadline.set(Some(Instant::now() + recording.duration));
                recording.started();
            }
            StreamState::Error(_) => state_error.set(Some(OsError::Backend)),
            StreamState::Unconnected | StreamState::Paused if old == StreamState::Streaming => {
                state_error.set(Some(OsError::Backend));
            }
            _ => {}
        })
        .process(move |stream, recording| {
            let Some(mut buffer) = stream.dequeue_buffer() else {
                return;
            };
            if data_error.get().is_some()
                || data_deadline.get().is_none_or(|end| Instant::now() >= end)
            {
                return;
            }
            let Some(data) = buffer.datas_mut().first_mut() else {
                return;
            };
            let offset = data.chunk().offset() as usize;
            let size = data.chunk().size() as usize;
            if size == 0 {
                return;
            }
            let pcm = data
                .data()
                .and_then(|bytes| bytes.get(offset..offset.saturating_add(size)));
            let result = match pcm {
                Some(pcm) if size & 1 == 0 && size <= MAX_CHUNK => recording.borrow().push(pcm),
                _ => Err(OsError::Backend),
            };
            if let Err(reason) = result {
                data_error.set(Some(reason));
            }
        })
        .register()
        .map_err(|_| OsError::Backend)?;

    // PipeWire 在流与设备之间重采样、混为单声道并转换为 S16LE。
    let mut format = spa::param::audio::AudioInfoRaw::new();
    format.set_format(spa::param::audio::AudioFormat::S16LE);
    format.set_rate(RATE);
    format.set_channels(1);
    let mut position = [0; 64];
    position[0] = spa::sys::SPA_AUDIO_CHANNEL_MONO;
    format.set_position(position);
    let value = spa::pod::Value::Object(spa::pod::Object {
        type_: spa::utils::SpaTypes::ObjectParamFormat.as_raw(),
        id: spa::param::ParamType::EnumFormat.as_raw(),
        properties: format.into(),
    });
    let (bytes, _) = spa::pod::serialize::PodSerializer::serialize(Cursor::new(Vec::new()), &value)
        .map_err(|_| OsError::Backend)?;
    let pod = spa::pod::Pod::from_bytes(bytes.get_ref()).ok_or(OsError::Backend)?;
    stream
        .connect(
            spa::utils::Direction::Input,
            None,
            StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
            &mut [pod],
        )
        .map_err(|_| OsError::Backend)?;

    loop {
        if let Some(error) = error.get() {
            return Err(error);
        }
        if disconnected.get() {
            return Err(OsError::Backend);
        }
        if chunks.is_closed() || stopping.load(std::sync::atomic::Ordering::Acquire) {
            return Err(OsError::Unavailable);
        }
        let end = deadline.get();
        let remaining = end
            .unwrap_or(startup_deadline)
            .saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return if end.is_some() {
                Ok(())
            } else {
                Err(OsError::Timeout)
            };
        }
        let timeout = pw::loop_::Timeout::Finite(remaining.min(POLL_INTERVAL));
        let result = main_loop.loop_().iterate(timeout);
        if result < 0 && result != -libc::EINTR {
            return Err(OsError::Backend);
        }
    }
}
