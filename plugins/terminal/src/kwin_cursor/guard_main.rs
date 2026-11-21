//! Independent cleanup for a provider killed with SIGKILL.

#[allow(dead_code)]
mod bus;

use std::ffi::CString;
use std::io::{self, Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::process::{Command, ExitCode};
use std::thread;
use std::time::Duration;

fn main() -> ExitCode {
    if run().is_ok() {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

fn run() -> io::Result<()> {
    close_inherited_fds()?;
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let [mode, owner, client, script] = args.as_slice() else {
        return Err(io::Error::other("invalid cursor guard arguments"));
    };
    let owner = CString::new(owner.as_bytes())?;
    let client = CString::new(client.as_bytes())?;
    let script = CString::new(script.as_bytes())?;
    let client = client.as_c_str();
    let owner = owner.as_c_str();
    if !owner.to_bytes().starts_with(b":")
        || !client.to_bytes().starts_with(b":")
        || !script.to_bytes().starts_with(b"deskwaifu-cursor-")
    {
        return Err(io::Error::other("invalid cursor guard identifiers"));
    }
    if mode == "launch" {
        // The host reaps this launcher. The detached child inherits only the
        // control and acknowledgement streams, so it can finish after host exit.
        Command::new(std::env::current_exe()?)
            .arg("watch")
            .args(&args[1..])
            .spawn()?;
        return Ok(());
    }
    if mode != "watch" {
        return Err(io::Error::other("invalid cursor guard mode"));
    }
    rustix::process::setsid()?;
    let bus = bus::Bus::open()?;
    if !bus.has_owner(client)? || !bus.has_owner(owner)? {
        return Ok(());
    }
    {
        let mut status = io::stdout().lock();
        status.write_all(b"R")?;
        status.flush()?;
    }
    // EOF signals host exit or the provider ending this attempt. Wait for the
    // broker to disconnect the provider, placing pending load ahead of unload.
    let _ = io::stdin().read(&mut [0]);
    loop {
        match bus.has_owner(owner) {
            Ok(false) => break,
            Ok(true) => {
                if matches!(bus.has_owner(client), Ok(false)) && bus.unload(owner, &script).is_ok()
                {
                    break;
                }
            }
            Err(_) if !bus.is_open() => break,
            Err(_) => {}
        }
        thread::sleep(Duration::from_millis(200));
    }
    Ok(())
}

#[allow(unsafe_code)]
fn close_inherited_fds() -> io::Result<()> {
    unsafe extern "C" {
        fn close(fd: std::ffi::c_int) -> std::ffi::c_int;
    }
    // Take the snapshot before closing any descriptors. The directory FD has
    // already been closed when iteration completes, and EBADF is harmless.
    let mut inherited = Vec::new();
    for entry in std::fs::read_dir("/proc/self/fd")? {
        let path = entry?.file_name();
        if let Some(fd) = path.to_str().and_then(|s| s.parse::<i32>().ok()) {
            if fd > 2 {
                inherited.push(fd);
            }
        }
    }
    for fd in inherited {
        // SAFETY: the guard is single-threaded here. No Rust-owned descriptor
        // remains live above stderr and no new descriptors are opened in this loop.
        // C's close also accepts a stale descriptor number and reports EBADF.
        unsafe { close(fd) };
    }
    Ok(())
}
