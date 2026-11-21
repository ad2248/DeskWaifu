//! Narrow, thread-confined sd-bus bindings shared with the crash guard.
#![allow(unsafe_code)]

use std::ffi::{CStr, c_char, c_int, c_void};
use std::io;
use std::ptr::{NonNull, null_mut};

#[repr(C)]
struct RawBus(c_void);
#[repr(C)]
struct RawMessage(c_void);

#[link(name = "systemd")]
unsafe extern "C" {
    fn sd_bus_open_user(bus: *mut *mut RawBus) -> c_int;
    fn sd_bus_close_unref(bus: *mut RawBus) -> *mut RawBus;
    fn sd_bus_is_open(bus: *mut RawBus) -> c_int;
    fn sd_bus_set_method_call_timeout(bus: *mut RawBus, timeout: u64) -> c_int;
    fn sd_bus_get_unique_name(bus: *mut RawBus, name: *mut *const c_char) -> c_int;
    fn sd_bus_call_method(
        bus: *mut RawBus,
        dest: *const c_char,
        path: *const c_char,
        interface: *const c_char,
        method: *const c_char,
        error: *mut c_void,
        reply: *mut *mut RawMessage,
        signature: *const c_char,
        ...
    ) -> c_int;
    fn sd_bus_process(bus: *mut RawBus, result: *mut *mut RawMessage) -> c_int;
    fn sd_bus_wait(bus: *mut RawBus, timeout: u64) -> c_int;
    fn sd_bus_message_unref(message: *mut RawMessage) -> *mut RawMessage;
    fn sd_bus_message_get_sender(message: *mut RawMessage) -> *const c_char;
    fn sd_bus_message_get_path(message: *mut RawMessage) -> *const c_char;
    fn sd_bus_message_is_method_call(
        message: *mut RawMessage,
        interface: *const c_char,
        member: *const c_char,
    ) -> c_int;
    fn sd_bus_message_has_signature(message: *mut RawMessage, signature: *const c_char) -> c_int;
    fn sd_bus_message_read_basic(
        message: *mut RawMessage,
        kind: c_char,
        result: *mut c_void,
    ) -> c_int;
    fn sd_bus_reply_method_return(call: *mut RawMessage, signature: *const c_char, ...) -> c_int;
    fn sd_bus_reply_method_errorf(
        call: *mut RawMessage,
        name: *const c_char,
        format: *const c_char,
        ...
    ) -> c_int;
}

const DAEMON: &CStr = c"org.freedesktop.DBus";
const DAEMON_PATH: &CStr = c"/org/freedesktop/DBus";
const SCRIPTING: &CStr = c"org.kde.kwin.Scripting";
const SCRIPTING_PATH: &CStr = c"/Scripting";

fn checked(result: c_int) -> io::Result<c_int> {
    if result < 0 {
        Err(io::Error::from_raw_os_error(-result))
    } else {
        Ok(result)
    }
}

pub(super) struct Bus(NonNull<RawBus>);

impl Bus {
    pub(super) fn open() -> io::Result<Self> {
        let mut ptr = null_mut();
        // SAFETY: out-pointer is writable. Successful open provides one owned reference.
        checked(unsafe { sd_bus_open_user(&mut ptr) })?;
        let bus = Self(NonNull::new(ptr).ok_or_else(|| io::Error::other("missing session bus"))?);
        // SAFETY: bus owns a live sd_bus reference.
        checked(unsafe { sd_bus_set_method_call_timeout(bus.0.as_ptr(), 250_000) })?;
        Ok(bus)
    }

    #[allow(dead_code)] // Used by the companion binary.
    pub(super) fn is_open(&self) -> bool {
        // SAFETY: this wrapper owns the connection, even if the transport closed.
        unsafe { sd_bus_is_open(self.0.as_ptr()) > 0 }
    }

    pub(super) fn own_name(&self) -> io::Result<&CStr> {
        let mut ptr = std::ptr::null();
        // SAFETY: bus is live and the returned string remains valid for this connection.
        checked(unsafe { sd_bus_get_unique_name(self.0.as_ptr(), &mut ptr) })?;
        if ptr.is_null() {
            return Err(io::Error::other("missing unique bus name"));
        }
        Ok(unsafe { CStr::from_ptr(ptr) })
    }

    pub(super) fn owner(&self) -> io::Result<std::ffi::CString> {
        self.call(
            DAEMON,
            DAEMON_PATH,
            DAEMON,
            c"GetNameOwner",
            &[c"org.kde.KWin"],
        )?
        .read_string()
    }

    #[allow(dead_code)] // Used by the companion binary.
    pub(super) fn has_owner(&self, name: &CStr) -> io::Result<bool> {
        self.call(DAEMON, DAEMON_PATH, DAEMON, c"NameHasOwner", &[name])?
            .read_bool()
    }

    pub(super) fn load(&self, owner: &CStr, file: &CStr, script: &CStr) -> io::Result<i32> {
        self.call(
            owner,
            SCRIPTING_PATH,
            SCRIPTING,
            c"loadScript",
            &[file, script],
        )?
        .read_i32()
    }

    pub(super) fn run(&self, owner: &CStr, path: &CStr) -> io::Result<()> {
        self.call(owner, path, c"org.kde.kwin.Script", c"run", &[])
            .map(drop)
    }

    #[allow(dead_code)] // Used by the companion binary.
    pub(super) fn unload(&self, owner: &CStr, script: &CStr) -> io::Result<()> {
        self.call(owner, SCRIPTING_PATH, SCRIPTING, c"unloadScript", &[script])
            .map(drop)
    }

    fn call(
        &self,
        dest: &CStr,
        path: &CStr,
        interface: &CStr,
        method: &CStr,
        args: &[&CStr],
    ) -> io::Result<Message> {
        let mut reply = null_mut();
        let (bus, dest, path, interface, method) = (
            self.0.as_ptr(),
            dest.as_ptr(),
            path.as_ptr(),
            interface.as_ptr(),
            method.as_ptr(),
        );
        let error = null_mut();
        // SAFETY: all borrowed C strings live through the call. The signature always matches
        // the variadic pointers. A successful call returns one owned message reference.
        let result = unsafe {
            match args {
                [] => sd_bus_call_method(
                    bus,
                    dest,
                    path,
                    interface,
                    method,
                    error,
                    &mut reply,
                    c"".as_ptr(),
                ),
                [a] => sd_bus_call_method(
                    bus,
                    dest,
                    path,
                    interface,
                    method,
                    error,
                    &mut reply,
                    c"s".as_ptr(),
                    a.as_ptr(),
                ),
                [a, b] => sd_bus_call_method(
                    bus,
                    dest,
                    path,
                    interface,
                    method,
                    error,
                    &mut reply,
                    c"ss".as_ptr(),
                    a.as_ptr(),
                    b.as_ptr(),
                ),
                _ => unreachable!("private call supports up to two strings"),
            }
        };
        checked(result)?;
        NonNull::new(reply)
            .map(Message)
            .ok_or_else(|| io::Error::other("missing D-Bus reply"))
    }

    pub(super) fn process(&self) -> io::Result<(bool, Option<Message>)> {
        let mut ptr = null_mut();
        // SAFETY: bus is alive and the out-pointer is writable.
        let progress = checked(unsafe { sd_bus_process(self.0.as_ptr(), &mut ptr) })? > 0;
        Ok((progress, NonNull::new(ptr).map(Message)))
    }

    pub(super) fn wait(&self) -> io::Result<()> {
        // SAFETY: bus is alive. The duration is in microseconds.
        checked(unsafe { sd_bus_wait(self.0.as_ptr(), 50_000) }).map(drop)
    }
}

impl Drop for Bus {
    fn drop(&mut self) {
        // SAFETY: this wrapper owns one reference and closes the connection exactly once.
        unsafe {
            sd_bus_close_unref(self.0.as_ptr());
        }
    }
}

pub(super) struct Message(NonNull<RawMessage>);

impl Message {
    pub(super) fn is_sample(&self) -> bool {
        // SAFETY: self holds a live message reference. A non-null path is message-owned.
        unsafe {
            if sd_bus_message_is_method_call(
                self.0.as_ptr(),
                c"org.deskwaifu.Cursor".as_ptr(),
                c"Sample".as_ptr(),
            ) <= 0
            {
                return false;
            }
            let path = sd_bus_message_get_path(self.0.as_ptr());
            !path.is_null() && CStr::from_ptr(path) == c"/org/deskwaifu/Cursor"
        }
    }

    pub(super) fn sender_is(&self, name: &CStr) -> bool {
        // SAFETY: a non-null sender is a NUL-terminated message-owned string.
        unsafe {
            let ptr = sd_bus_message_get_sender(self.0.as_ptr());
            !ptr.is_null() && CStr::from_ptr(ptr) == name
        }
    }

    pub(super) fn has_sample_signature(&self) -> bool {
        // SAFETY: self holds a live message reference and the signature is a C string.
        unsafe { sd_bus_message_has_signature(self.0.as_ptr(), c"ssssssss".as_ptr()) > 0 }
    }

    pub(super) fn read_string(&mut self) -> io::Result<std::ffi::CString> {
        let mut ptr: *const c_char = std::ptr::null();
        // SAFETY: the signature 's' expects an out-pointer to a borrowed C string.
        let result = unsafe {
            sd_bus_message_read_basic(
                self.0.as_ptr(),
                b's' as c_char,
                (&mut ptr as *mut *const c_char).cast(),
            )
        };
        if checked(result)? == 0 || ptr.is_null() {
            return Err(io::Error::other("missing D-Bus string"));
        }
        // SAFETY: the message holds the string through the copy.
        Ok(unsafe { CStr::from_ptr(ptr) }.to_owned())
    }

    fn read_i32(&mut self) -> io::Result<i32> {
        self.read_int(b'i')
    }

    fn read_bool(&mut self) -> io::Result<bool> {
        self.read_int(b'b').map(|v| v != 0)
    }

    fn read_int(&mut self, kind: u8) -> io::Result<i32> {
        let mut value: i32 = 0;
        // SAFETY: the only callers pass 'i' or 'b', both read into an int on Linux.
        let result = unsafe {
            sd_bus_message_read_basic(
                self.0.as_ptr(),
                kind as c_char,
                (&mut value as *mut i32).cast(),
            )
        };
        if checked(result)? == 0 {
            return Err(io::Error::other("missing D-Bus number"));
        }
        Ok(value)
    }

    pub(super) fn reply(&self, value: &CStr) -> io::Result<()> {
        // SAFETY: signature 's' describes the one live C string argument.
        checked(unsafe {
            sd_bus_reply_method_return(self.0.as_ptr(), c"s".as_ptr(), value.as_ptr())
        })
        .map(drop)
    }

    pub(super) fn reject(&self, error: &CStr, reason: &CStr) -> io::Result<()> {
        // SAFETY: one live C string argument for the fixed "%s" format.
        checked(unsafe {
            sd_bus_reply_method_errorf(
                self.0.as_ptr(),
                error.as_ptr(),
                c"%s".as_ptr(),
                reason.as_ptr(),
            )
        })
        .map(drop)
    }
}

impl Drop for Message {
    fn drop(&mut self) {
        // SAFETY: this wrapper owns one message reference.
        unsafe {
            sd_bus_message_unref(self.0.as_ptr());
        }
    }
}
