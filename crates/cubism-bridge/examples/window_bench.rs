//! Run inside a private Wayland compositor. See the crate README for commands.
use std::{
    env,
    error::Error,
    fs::File,
    io::{self, BufWriter},
    os::unix::net::UnixDatagram,
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
    thread,
    time::{Duration, Instant},
};

use cubism_bridge::{Runtime, rgba_len, unpremultiply_rgba};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

// Keep the example's libwayland FFI and lifetime checks in this module.
#[allow(unsafe_code)]
mod wayland {
    use super::{Result, Runtime, echo, rgba_len};
    use std::{
        ffi::{CStr, c_char, c_void},
        os::fd::AsRawFd,
        os::unix::net::UnixDatagram,
        ptr::{self, NonNull},
        time::{Duration, Instant},
    };
    unsafe extern "C" {
        fn dw_error() -> *const c_char;
        fn dw_probe_open(
            width: i32,
            height: i32,
            egl: i32,
            layer: *mut *mut c_void,
            display: *mut *mut c_void,
            window: *mut *mut c_void,
        ) -> i32;
        fn dw_probe_wait(layer: *mut c_void, fd: i32, ms: i32, flags: *mut i32) -> i32;
        fn dw_probe_prepare(layer: *mut c_void, rects: *const [i32; 4], count: usize) -> i32;
        fn dw_probe_commit_shm(layer: *mut c_void, rgba: *const u8) -> i32;
        fn dw_probe_close(layer: *mut c_void);
    }

    fn check(status: i32) -> Result<()> {
        if status == 0 {
            return Ok(());
        }
        // Copy the shim's non-null, thread-local NUL-terminated error string.
        Err(unsafe { CStr::from_ptr(dw_error()) }
            .to_string_lossy()
            .into_owned()
            .into())
    }

    pub struct Layer {
        raw: NonNull<c_void>,
        display: *mut c_void,
        window: *mut c_void,
        size: u32,
    }

    impl Layer {
        pub fn open(size: u32, egl: bool) -> Result<Self> {
            let (mut raw, mut display, mut window) =
                (ptr::null_mut(), ptr::null_mut(), ptr::null_mut());
            // The shim returns owned layer state and its two borrowed Wayland pointers.
            check(unsafe {
                dw_probe_open(
                    size as i32,
                    size as i32,
                    i32::from(egl),
                    &mut raw,
                    &mut display,
                    &mut window,
                )
            })?;
            Ok(Self {
                raw: NonNull::new(raw).ok_or("missing layer state")?,
                display,
                window,
                size,
            })
        }

        pub fn runtime(&self) -> Result<LayerRuntime<'_>> {
            let runtime = if self.window.is_null() {
                Runtime::new(self.size, self.size)?
            } else {
                // The caller drops the runtime before the configured layer and its fixed-size window.
                unsafe {
                    Runtime::new_wayland_window(self.size, self.size, self.display, self.window)?
                }
            };
            Ok(LayerRuntime {
                runtime,
                _layer: self,
            })
        }

        pub fn next(&self, ipc: &UnixDatagram) -> Result<()> {
            let start = Instant::now();
            loop {
                let mut flags = 0;
                // The layer and descriptor remain live across the Wayland + IPC poll.
                check(unsafe {
                    dw_probe_wait(self.raw.as_ptr(), ipc.as_raw_fd(), 100, &mut flags)
                })?;
                echo(ipc)?;
                if flags & 1 != 0 {
                    return Ok(());
                }
                if start.elapsed() > Duration::from_secs(5) {
                    return Err("Wayland frame callback or buffer release timed out".into());
                }
            }
        }

        pub fn present(&self, runtime: &Runtime, rects: &[[i32; 4]], rgba: &[u8]) -> Result<()> {
            // The shim synchronously reads exactly rects.len() rectangles.
            check(unsafe { dw_probe_prepare(self.raw.as_ptr(), rects.as_ptr(), rects.len()) })?;
            if self.window.is_null() {
                assert_eq!(rgba.len(), rgba_len(self.size, self.size).unwrap());
                // The shim reads the entire validated RGBA frame into a released buffer.
                check(unsafe { dw_probe_commit_shm(self.raw.as_ptr(), rgba.as_ptr()) })
            } else {
                Ok(runtime.present_window()?)
            }
        }
    }

    impl Drop for Layer {
        fn drop(&mut self) {
            // EGL is dropped before the Wayland layer by declaration order in main.
            unsafe { dw_probe_close(self.raw.as_ptr()) }
        }
    }

    pub struct LayerRuntime<'a> {
        runtime: Runtime,
        _layer: &'a Layer,
    }

    impl std::ops::Deref for LayerRuntime<'_> {
        type Target = Runtime;

        fn deref(&self) -> &Runtime {
            &self.runtime
        }
    }
}
use wayland::Layer;

fn echo(ipc: &UnixDatagram) -> io::Result<()> {
    let mut message = [0; 8];
    match ipc.recv(&mut message) {
        Ok(n) => ipc.send(&message[..n]).map(drop),
        Err(error) if error.kind() == io::ErrorKind::WouldBlock => Ok(()),
        Err(error) => Err(error),
    }
}

// Merge adjacent occupied cells in each row. Coordinates are logical pixels at scale 1.
fn region(size: u32, grid: u32, mut alpha: impl FnMut(u32, u32) -> u8, out: &mut Vec<[i32; 4]>) {
    out.clear();
    let edge = |i| (i * size / grid) as i32;
    for y in 0..grid {
        let mut start = None;
        for x in 0..=grid {
            if x < grid && alpha(x, y) >= 16 {
                start.get_or_insert(x);
            } else if let Some(begin) = start.take() {
                out.push([
                    edge(begin),
                    edge(y),
                    edge(x) - edge(begin),
                    edge(y + 1) - edge(y),
                ]);
            }
        }
    }
}

fn texture_pixel(size: u32, grid: u32, x: u32, y: u32) -> usize {
    let center = |i| (2 * i + 1) * size / (2 * grid);
    // GL nearest samples from the bottom left. Mirror y to match its tie direction.
    ((size - 1 - center(grid - 1 - y)) * size + center(x)) as usize * 4
}

fn save_px(path: &Path, size: u32, rgba: &[u8]) -> Result<()> {
    let mut png_pixels = rgba.to_vec();
    let mut transparent = 0;
    let mut translucent = 0;
    let mut opaque = 0;
    for chunk in png_pixels.chunks_exact_mut(4) {
        let pixel: &mut [u8; 4] = chunk.try_into().unwrap();
        match pixel[3] {
            0 => transparent += 1,
            255 => opaque += 1,
            _ => translucent += 1,
        }
        if pixel[..3]
            .iter()
            .any(|&channel| channel > pixel[3].saturating_add(1))
        {
            return Err("readback contains non-premultiplied pixels".into());
        }
        unpremultiply_rgba(std::slice::from_mut(pixel));
    }
    if transparent == 0 || translucent == 0 || opaque == 0 {
        return Err("readback lacks transparent, partial, or opaque alpha".into());
    }
    let mut encoder = png::Encoder::new(BufWriter::new(File::create(path)?), size, size);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header()?.write_image_data(&png_pixels)?;
    println!("verified_pixels transparent={transparent} translucent={translucent} opaque={opaque}");
    Ok(())
}

fn stats(label: &str, values: &mut [Duration]) {
    values.sort_unstable();
    if values.is_empty() {
        println!("{label}_ms count=0");
        return;
    }
    let at = |p: usize| values[(values.len() * p).div_ceil(100) - 1].as_secs_f64() * 1000.0;
    println!(
        "{label}_ms count={} p50={:.3} p95={:.3} p99={:.3} max={:.3}",
        values.len(),
        at(50),
        at(95),
        at(99),
        at(100)
    );
}

fn main() -> Result<()> {
    let usage = "usage: window_bench <egl|shm> MODEL.model3.json <SIZE> <FRAMES> [VERIFY.png]";
    let mut args = env::args_os().skip(1);
    let egl = match args.next().as_deref().and_then(|arg| arg.to_str()) {
        Some("egl") => true,
        Some("shm") => false,
        _ => return Err(usage.into()),
    };
    let path = PathBuf::from(args.next().ok_or(usage)?);
    let size: u32 = args.next().ok_or(usage)?.to_str().ok_or(usage)?.parse()?;
    let frames: usize = args.next().ok_or(usage)?.to_str().ok_or(usage)?.parse()?;
    let snapshot = args.next().map(PathBuf::from);
    if !(16..=2048).contains(&size) || !(1..=3600).contains(&frames) || args.next().is_some() {
        return Err(usage.into());
    }
    let layer = Layer::open(size, egl)?;
    let runtime = layer.runtime()?;
    let mut model = runtime.load(&path)?;
    if model
        .motion_groups()?
        .iter()
        .any(|(group, count)| group == "Idle" && *count > 1)
    {
        model.play_motion("Idle", 1)?;
    }
    println!(
        "renderer={} masking={}",
        runtime.renderer(),
        model.uses_masking()
    );
    let (grid, _) = runtime.window_mask_size();
    let mut full = vec![
        0;
        if !egl || snapshot.is_some() {
            rgba_len(size, size).unwrap()
        } else {
            0
        }
    ];
    let mut sampled = vec![
        0;
        if egl {
            rgba_len(grid, grid).unwrap()
        } else {
            0
        }
    ];
    let mut rects = Vec::new();
    let (worker, client) = UnixDatagram::pair()?;
    worker.set_nonblocking(true)?;
    client.set_read_timeout(Some(Duration::from_millis(100)))?;
    let mut frame = |snapshot: Option<&Path>| -> Result<_> {
        layer.next(&worker)?;
        let begin = Instant::now();
        let dt = 1.0 / 60.0;
        if egl {
            model.draw_window(dt)?;
            runtime.read_window_mask(&mut sampled)?;
            region(
                size,
                grid,
                |x, y| sampled[(y * grid + x) as usize * 4 + 3],
                &mut rects,
            );
        } else {
            model.render(dt, &mut full)?;
            region(
                size,
                grid,
                |x, y| full[texture_pixel(size, grid, x, y) + 3],
                &mut rects,
            );
        }
        if let Some(path) = snapshot {
            if egl {
                runtime.read_window_pixels(&mut full)?;
            }
            save_px(path, size, &full)?;
        }
        let commit = Instant::now();
        layer.present(&runtime, &rects, &full)?;
        let end = Instant::now();
        Ok((begin, end - begin, end - commit))
    };
    const WARMUP: usize = 20;
    frame(snapshot.as_deref())?;
    for _ in 1..WARMUP {
        frame(None)?;
    }
    layer.next(&worker)?;
    let done = AtomicBool::new(false);
    let (mut work, mut present, mut cadence) = (Vec::new(), Vec::new(), Vec::new());
    let total = thread::scope(|scope| -> Result<_> {
        let ipc = scope.spawn(|| {
            let mut latencies = Vec::new();
            let mut misses = 0;
            let mut id = 0u64;
            while !done.load(Ordering::Relaxed) {
                let start = Instant::now();
                let mut reply = [0; 8];
                id += 1;
                if client.send(&id.to_ne_bytes()).is_ok() {
                    // Discard any late reply to the previous, timed-out request.
                    loop {
                        match client.recv(&mut reply) {
                            Ok(8) if u64::from_ne_bytes(reply) == id => {
                                latencies.push(start.elapsed());
                                break;
                            }
                            Ok(_) if start.elapsed() < Duration::from_millis(100) => continue,
                            _ => break,
                        }
                    }
                }
                if latencies.len() + misses < id as usize && !done.load(Ordering::Relaxed) {
                    misses += 1;
                }
                thread::sleep(Duration::from_millis(2));
            }
            (latencies, misses)
        });
        let result = (|| -> Result<Duration> {
            let started = Instant::now();
            let mut previous = None;
            for _ in 0..frames {
                let (begin, elapsed, commit_elapsed) = frame(None)?;
                work.push(elapsed);
                present.push(commit_elapsed);
                if let Some(previous) = previous.replace(begin) {
                    cadence.push(begin - previous);
                }
            }
            layer.next(&worker)?;
            Ok(started.elapsed())
        })();
        done.store(true, Ordering::Relaxed);
        echo(&worker)?;
        let (mut latencies, misses) = ipc.join().map_err(|_| "IPC peer panicked")?;
        stats("ipc_rtt", &mut latencies);
        println!("ipc_misses={misses}");
        result
    })?;
    stats("frame_work", &mut work);
    stats("present", &mut present);
    stats("frame_interval", &mut cadence);
    let bytes = if egl { sampled.len() } else { full.len() };
    println!(
        "path={} size={size} frames={frames} warmup={WARMUP} measured_s={:.3} readback_bytes_per_frame={bytes}",
        if egl { "egl" } else { "shm" },
        total.as_secs_f64()
    );
    Ok(())
}
