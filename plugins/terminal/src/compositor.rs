//! Read-only queries against compositor control sockets. No child processes,
//! Wayland output changes, or fallback guesses are performed here.

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum FocusedOutputError {
    Unsupported,
    Unavailable(String),
    Malformed(String),
}

impl std::fmt::Display for FocusedOutputError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Unsupported => {
                f.write_str("compositor capability is unsupported in this session")
            }
            Self::Unavailable(reason) => {
                write!(f, "compositor capability is unavailable: {reason}")
            }
            Self::Malformed(reason) => write!(f, "invalid compositor response: {reason}"),
        }
    }
}

impl std::error::Error for FocusedOutputError {}

/// Return the exact connector name from the session compositor, without rebinding
/// or persisting anything. One 250 ms deadline covers connect, write and all reads;
/// JSON payloads must be smaller than 16 KiB. A valid reply with no focused output
/// is unavailable, not malformed.
pub(crate) fn focused_output() -> Result<String, FocusedOutputError> {
    #[cfg(target_os = "linux")]
    {
        use std::env::var_os;

        linux::target(
            var_os("HYPRLAND_INSTANCE_SIGNATURE").as_deref(),
            var_os("XDG_RUNTIME_DIR").as_deref(),
            var_os("SWAYSOCK").as_deref(),
        )?
        .query()
    }
    #[cfg(not(target_os = "linux"))]
    Err(FocusedOutputError::Unsupported)
}

/// A sampled Hyprland cursor and its unique target monitor. Both positions are
/// integer global Wayland logical coordinates, not physical buffer pixels.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct HyprlandPointer {
    pub(crate) position: (i32, i32),
    pub(crate) output: String,
    pub(crate) output_origin: (i32, i32),
}

/// Sample the Hyprland cursor and find the monitor containing it. Cursor and
/// layout replies are sequential, not an atomic snapshot; the caller must check
/// that the returned connector still exists in its active Wayland output list.
/// Both IPC requests share one 250 ms deadline and each reply must be smaller
/// than 16 KiB. There is no fallback for layout gaps or ambiguous destinations.
pub(crate) fn hyprland_pointer() -> Result<HyprlandPointer, FocusedOutputError> {
    #[cfg(target_os = "linux")]
    {
        use std::env::var_os;

        linux::target(
            var_os("HYPRLAND_INSTANCE_SIGNATURE").as_deref(),
            var_os("XDG_RUNTIME_DIR").as_deref(),
            None,
        )?
        .pointer()
    }
    #[cfg(not(target_os = "linux"))]
    Err(FocusedOutputError::Unsupported)
}

#[cfg(target_os = "linux")]
mod linux {
    use super::FocusedOutputError::{self, Malformed, Unavailable, Unsupported};
    use super::HyprlandPointer;
    use rustix::event::{PollFd, PollFlags, Timespec, poll};
    use rustix::io::{Errno, read};
    use rustix::net::{
        AddressFamily, SendFlags, SocketAddrUnix, SocketFlags, SocketType, connect, send,
        socket_with, sockopt,
    };
    use serde::Deserialize;
    use std::ffi::OsStr;
    use std::os::fd::OwnedFd;
    use std::path::{Path, PathBuf};
    use std::time::{Duration, Instant};

    const QUERY_BUDGET: Duration = Duration::from_millis(250);
    const RESPONSE_LIMIT: usize = 16 * 1024;
    const SWAY_MAGIC: &[u8; 6] = b"i3-ipc";
    // i3/Sway headers have six magic bytes followed by native-endian u32 length and type.
    const SWAY_GET_OUTPUTS: u32 = 3;
    const SWAY_HEADER_LEN: usize = 14;
    const HYPR_MONITORS: &[u8] = b"j/monitors";
    const HYPR_CURSOR: &[u8] = b"j/cursorpos";

    #[derive(Debug, PartialEq, Eq)]
    pub(super) enum Target {
        Sway(PathBuf),
        Hyprland(PathBuf),
    }

    pub(super) fn target(
        hypr_signature: Option<&OsStr>,
        runtime: Option<&OsStr>,
        sway_socket: Option<&OsStr>,
    ) -> Result<Target, FocusedOutputError> {
        // HYPRLAND_INSTANCE_SIGNATURE takes precedence over SWAYSOCK. I3SOCK
        // is ignored because it can identify i3 on X11.
        if let Some(signature) = hypr_signature {
            if Path::new(signature).file_name() != Some(signature) {
                return Err(Unavailable("invalid HYPRLAND_INSTANCE_SIGNATURE".into()));
            }
            let runtime = runtime
                .map(Path::new)
                .filter(|dir| dir.is_absolute())
                .ok_or_else(|| Unavailable("Hyprland needs an absolute XDG_RUNTIME_DIR".into()))?;
            return Ok(Target::Hyprland(
                runtime.join("hypr").join(signature).join(".socket.sock"),
            ));
        }
        if let Some(path) = sway_socket {
            let path = Path::new(path);
            if !path.is_absolute() {
                return Err(Unavailable(
                    "SWAYSOCK must be an absolute socket path".into(),
                ));
            }
            return Ok(Target::Sway(path.to_owned()));
        }
        Err(Unsupported)
    }

    impl Target {
        pub(super) fn query(&self) -> Result<String, FocusedOutputError> {
            let deadline = Instant::now() + QUERY_BUDGET;
            let response = match self {
                Self::Sway(path) => sway_outputs(&connect_to(path, deadline)?, deadline)?,
                Self::Hyprland(path) => hypr_request(path, HYPR_MONITORS, deadline)?,
            };
            parse_focused_output(&response)
        }

        pub(super) fn pointer(&self) -> Result<HyprlandPointer, FocusedOutputError> {
            let Self::Hyprland(path) = self else {
                return Err(Unsupported);
            };
            let deadline = Instant::now() + QUERY_BUDGET;
            let cursor = hypr_request(path, HYPR_CURSOR, deadline)?;
            let Cursor { x, y, .. } = serde_json::from_slice(&cursor)
                .map_err(|error| Malformed(format!("invalid Hyprland cursor position: {error}")))?;
            let monitors = hypr_request(path, HYPR_MONITORS, deadline)?;
            monitor_at((x, y), &monitors)
        }
    }

    fn hypr_request(
        path: &Path,
        request: &[u8],
        deadline: Instant,
    ) -> Result<Vec<u8>, FocusedOutputError> {
        if Instant::now() >= deadline {
            return Err(timed_out());
        }
        let fd = connect_to(path, deadline)?;
        send_request(&fd, request, deadline)?;
        read_to_eof(&fd, deadline)
    }

    fn io_error(operation: &str, error: Errno) -> FocusedOutputError {
        Unavailable(format!("{operation}: {error}"))
    }

    fn timed_out() -> FocusedOutputError {
        Unavailable("compositor IPC timed out".into())
    }

    fn wait_ready(
        fd: &OwnedFd,
        events: PollFlags,
        deadline: Instant,
    ) -> Result<(), FocusedOutputError> {
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(timed_out());
            }
            let timeout = Timespec::try_from(remaining).expect("250 ms fits in a timespec");
            match poll(&mut [PollFd::new(fd, events)], Some(&timeout)) {
                Ok(0) => return Err(timed_out()),
                // Let the caller inspect SO_ERROR or consume EOF on an error/hangup.
                Ok(_) if Instant::now() < deadline => return Ok(()),
                Ok(_) => return Err(timed_out()),
                Err(Errno::INTR) => continue,
                Err(error) => return Err(io_error("poll compositor socket", error)),
            }
        }
    }

    fn connect_to(path: &Path, deadline: Instant) -> Result<OwnedFd, FocusedOutputError> {
        let addr = SocketAddrUnix::new(path)
            .map_err(|error| io_error("invalid compositor socket path", error))?;
        let fd = socket_with(
            AddressFamily::UNIX,
            SocketType::STREAM,
            SocketFlags::CLOEXEC | SocketFlags::NONBLOCK,
            None,
        )
        .map_err(|error| io_error("create compositor socket", error))?;
        match connect(&fd, &addr) {
            Ok(()) => Ok(fd),
            Err(Errno::INPROGRESS) => {
                wait_ready(&fd, PollFlags::OUT, deadline)?;
                sockopt::socket_error(&fd)
                    .and_then(|result| result)
                    .map_err(|error| io_error("connect to compositor socket", error))?;
                Ok(fd)
            }
            // Linux UNIX sockets return EAGAIN for a full listen queue. Report it
            // rather than mistaking this for a pending connection or busy-looping.
            Err(error) => Err(io_error("connect to compositor socket", error)),
        }
    }

    fn send_request(
        fd: &OwnedFd,
        mut bytes: &[u8],
        deadline: Instant,
    ) -> Result<(), FocusedOutputError> {
        while !bytes.is_empty() {
            wait_ready(fd, PollFlags::OUT, deadline)?;
            match send(fd, bytes, SendFlags::NOSIGNAL) {
                Ok(0) => return Err(Unavailable("could not send compositor request".into())),
                Ok(count) => bytes = &bytes[count..],
                Err(Errno::INTR | Errno::AGAIN) => continue,
                Err(error) => return Err(io_error("write compositor request", error)),
            }
        }
        Ok(())
    }

    fn read_some(
        fd: &OwnedFd,
        bytes: &mut [u8],
        deadline: Instant,
    ) -> Result<usize, FocusedOutputError> {
        loop {
            wait_ready(fd, PollFlags::IN, deadline)?;
            match read(fd, &mut *bytes) {
                Ok(count) => return Ok(count),
                Err(Errno::INTR | Errno::AGAIN) => continue,
                Err(error) => return Err(io_error("read compositor response", error)),
            }
        }
    }

    fn read_exact(
        fd: &OwnedFd,
        mut bytes: &mut [u8],
        deadline: Instant,
    ) -> Result<(), FocusedOutputError> {
        while !bytes.is_empty() {
            let count = read_some(fd, bytes, deadline)?;
            if count == 0 {
                return Err(Malformed("truncated Sway IPC frame".into()));
            }
            bytes = &mut bytes[count..];
        }
        Ok(())
    }

    fn sway_outputs(fd: &OwnedFd, deadline: Instant) -> Result<Vec<u8>, FocusedOutputError> {
        let mut header = [0; SWAY_HEADER_LEN];
        header[..6].copy_from_slice(SWAY_MAGIC);
        header[10..].copy_from_slice(&SWAY_GET_OUTPUTS.to_ne_bytes());
        send_request(fd, &header, deadline)?;
        read_exact(fd, &mut header, deadline)?;
        if &header[..6] != SWAY_MAGIC || header[10..] != SWAY_GET_OUTPUTS.to_ne_bytes() {
            return Err(Malformed("unexpected Sway IPC response header".into()));
        }
        let len = u32::from_ne_bytes(header[6..10].try_into().expect("four length bytes")) as usize;
        if len >= RESPONSE_LIMIT {
            return Err(response_too_large());
        }
        // Sway frames have explicit lengths and the server can keep the socket open.
        let mut payload = vec![0; len];
        read_exact(fd, &mut payload, deadline)?;
        Ok(payload)
    }

    fn response_too_large() -> FocusedOutputError {
        Malformed("compositor payload reached the 16 KiB limit".into())
    }

    fn read_to_eof(fd: &OwnedFd, deadline: Instant) -> Result<Vec<u8>, FocusedOutputError> {
        let mut payload = Vec::new();
        let mut chunk = [0; 1024];
        loop {
            let capacity = chunk.len().min(RESPONSE_LIMIT - payload.len());
            let count = read_some(fd, &mut chunk[..capacity], deadline)?;
            if count == 0 {
                return Ok(payload);
            }
            payload.extend_from_slice(&chunk[..count]);
            if payload.len() == RESPONSE_LIMIT {
                return Err(response_too_large());
            }
        }
    }

    #[derive(Deserialize)]
    struct Cursor {
        x: i32,
        y: i32,
        #[serde(flatten)]
        _other: serde::de::IgnoredAny,
    }

    #[derive(Deserialize)]
    struct Monitor {
        name: String,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        scale: f64,
        transform: u8,
        #[serde(default)]
        disabled: bool,
        #[serde(rename = "mirrorOf")]
        mirror_of: Option<String>,
        #[serde(flatten)]
        _other: serde::de::IgnoredAny,
    }

    fn monitor_at(
        position: (i32, i32),
        response: &[u8],
    ) -> Result<HyprlandPointer, FocusedOutputError> {
        let monitors: Vec<Monitor> = serde_json::from_slice(response)
            .map_err(|error| Malformed(format!("invalid Hyprland monitor layout: {error}")))?;
        let mut matched = None;
        let mut ambiguous = false;
        let mut seen = std::collections::HashSet::new();
        for monitor in &monitors {
            if monitor.disabled
                || monitor
                    .mirror_of
                    .as_deref()
                    .is_some_and(|name| name != "none")
            {
                continue;
            }
            if !valid_output_name(&monitor.name) || !seen.insert(monitor.name.as_str()) {
                return Err(Malformed(
                    "invalid or duplicate Hyprland monitor name".into(),
                ));
            }
            if monitor.width == 0
                || monitor.height == 0
                || !monitor.scale.is_finite()
                || monitor.scale <= 0.0
                || monitor.transform > 7
            {
                return Err(Malformed("invalid Hyprland monitor geometry".into()));
            }
            // x/y are already logical origins. Width and height are mode pixels;
            // Hyprland swaps axes for odd Wayland transforms, divides by scale,
            // then rounds. Use half-open rectangles in global logical space.
            let (width, height) = if monitor.transform % 2 == 1 {
                (monitor.height, monitor.width)
            } else {
                (monitor.width, monitor.height)
            };
            let extent = |pixels: u32, scale: f64| (f64::from(pixels) / scale).round();
            let reported_width = extent(width, monitor.scale);
            let reported_height = extent(height, monitor.scale);
            if !(1.0..=f64::from(i32::MAX)).contains(&reported_width)
                || !(1.0..=f64::from(i32::MAX)).contains(&reported_height)
            {
                return Err(Malformed("invalid Hyprland logical monitor size".into()));
            }
            let dx = f64::from(position.0) - f64::from(monitor.x);
            let dy = f64::from(position.1) - f64::from(monitor.y);
            if dx < 0.0 || dy < 0.0 {
                continue;
            }
            // Hyprland serializes scale to two decimal places. Near a far edge,
            // a rounded scale may change which monitor contains a cursor pixel.
            // Reject such positions rather than falsely crossing to a neighbor.
            let max_scale = monitor.scale + 0.005;
            let min_scale = (monitor.scale - 0.005).max(f64::MIN_POSITIVE);
            let inside_min = dx < extent(width, max_scale) && dy < extent(height, max_scale);
            let inside_max = dx < extent(width, min_scale) && dy < extent(height, min_scale);
            if inside_min {
                ambiguous |= matched.replace(monitor).is_some();
            } else if inside_max {
                ambiguous = true;
            }
        }
        if ambiguous {
            return Err(Unavailable("cursor does not identify a unique Hyprland monitor (overlap or imprecise display edge)".into()));
        }
        let monitor = matched
            .ok_or_else(|| Unavailable("cursor is outside the Hyprland monitor layout".into()))?;
        Ok(HyprlandPointer {
            position,
            output: monitor.name.clone(),
            output_origin: (monitor.x, monitor.y),
        })
    }

    fn valid_output_name(name: &str) -> bool {
        !name.trim().is_empty() && !name.chars().any(char::is_control)
    }

    #[derive(Deserialize)]
    struct Output {
        name: String,
        focused: Option<bool>,
        // Sway omits `focused` on disabled and non-desktop outputs.
        active: Option<bool>,
        #[serde(default)]
        non_desktop: bool,
        // Require JSON object entries; Serde otherwise also accepts positional
        // arrays for a derived struct. Unrelated compositor fields are ignored.
        #[serde(flatten)]
        _other: serde::de::IgnoredAny,
    }

    fn parse_focused_output(response: &[u8]) -> Result<String, FocusedOutputError> {
        let outputs: Vec<Output> = serde_json::from_slice(response)
            .map_err(|error| Malformed(format!("invalid output list: {error}")))?;
        let mut focused = None;
        for output in outputs {
            if output.non_desktop || output.active == Some(false) {
                continue;
            }
            if output
                .focused
                .ok_or_else(|| Malformed("missing focused flag on an active output".into()))?
            {
                if !valid_output_name(&output.name) {
                    return Err(Malformed("invalid focused output name".into()));
                }
                if focused.replace(output.name).is_some() {
                    return Err(Malformed("multiple outputs are marked focused".into()));
                }
            }
        }
        focused.ok_or_else(|| Unavailable("no output is currently focused".into()))
    }
}
