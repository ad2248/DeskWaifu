//! One-shot, newline-framed Unix IPC. Requests are never retried.
use serde::{
    de::{MapAccess, SeqAccess, Visitor},
    Deserialize, Deserializer,
};
use serde_json::{Map, Number, Value};
use socket2::{Domain, SockAddr, Socket, Type};
use std::{
    env, fmt,
    io::{self, Read, Write},
    os::unix::net::UnixStream,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

pub const MAX_REQUEST: usize = 64 * 1024;
pub const MAX_RESPONSE: usize = 8 * 1024 * 1024;

pub fn default_socket_path() -> PathBuf {
    env::var_os("XDG_RUNTIME_DIR")
        .filter(|p| !p.is_empty())
        .map(|p| PathBuf::from(p).join("deskwaifu.sock"))
        .unwrap_or_else(|| "/tmp/deskwaifu.sock".into())
}

/// A valid server reply. `ok: false` is a server failure, not a transport error;
/// the complete JSON is retained so callers can display the diagnostic.
#[derive(Debug)]
pub struct Response {
    pub ok: bool,
    pub value: Value,
}

fn invalid(message: impl ToString) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message.to_string())
}
fn remaining(deadline: Instant) -> io::Result<Duration> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|d| !d.is_zero())
        .ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "IPC deadline exceeded"))
}

struct RequestBuffer(Vec<u8>);
impl Write for RequestBuffer {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > MAX_REQUEST - self.0.len() {
            return Err(invalid("Request exceeds 64 KiB"));
        }
        self.0.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Perform one exchange, with a single budget covering serialization, connection,
/// writes and reads. Limits exclude the framing newline. Partial I/O consumes the
/// same deadline, so a peer trickling bytes cannot extend the timeout indefinitely.
pub fn request(path: &Path, request: &Value, timeout: Duration) -> io::Result<Response> {
    let deadline = Instant::now()
        .checked_add(timeout)
        .ok_or_else(|| invalid("Timeout is too large"))?;
    let mut encoded = RequestBuffer(Vec::new());
    serde_json::to_writer(&mut encoded, request).map_err(invalid)?;
    encoded.0.push(b'\n');
    let socket = Socket::new(Domain::UNIX, Type::STREAM, None)?;
    socket.connect_timeout(&SockAddr::unix(path)?, remaining(deadline)?)?;
    let mut stream: UnixStream = unsafe {
        std::os::unix::io::FromRawFd::from_raw_fd(std::os::unix::io::IntoRawFd::into_raw_fd(socket))
    };
    let mut pending = encoded.0.as_slice();
    while !pending.is_empty() {
        stream.set_write_timeout(Some(remaining(deadline)?))?;
        match stream.write(pending) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "IPC write returned zero",
                ))
            }
            Ok(n) => pending = &pending[n..],
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    let mut line = Vec::new();
    let mut chunk = [0u8; 8192];
    loop {
        stream.set_read_timeout(Some(remaining(deadline)?))?;
        let n = match stream.read(&mut chunk) {
            Ok(0) => return Err(invalid("Truncated response (missing newline)")),
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        };
        let end = chunk[..n].iter().position(|b| *b == b'\n');
        let payload = &chunk[..end.unwrap_or(n)];
        if payload.len() > MAX_RESPONSE - line.len() {
            return Err(invalid("Response exceeds 8 MiB"));
        }
        line.extend_from_slice(payload);
        if end.is_some() {
            break;
        }
    }
    remaining(deadline)?;
    let value = serde_json::from_slice::<UniqueValue>(&line)
        .map_err(invalid)?
        .0;
    let ok = value
        .get("ok")
        .and_then(Value::as_bool)
        .ok_or_else(|| invalid("Response must be an object with a boolean 'ok' field"))?;
    remaining(deadline)?;
    Ok(Response { ok, value })
}

// Deserialize recursively rather than through Value (which silently replaces
// duplicate object keys). This also applies to objects inside arrays.
struct UniqueValue(Value);
impl<'de> Deserialize<'de> for UniqueValue {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        struct V;
        impl<'de> Visitor<'de> for V {
            type Value = UniqueValue;
            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str("JSON without duplicate keys")
            }
            fn visit_unit<E: serde::de::Error>(self) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::Null))
            }
            fn visit_bool<E: serde::de::Error>(self, v: bool) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_i64<E: serde::de::Error>(self, v: i64) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_u64<E: serde::de::Error>(self, v: u64) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_f64<E: serde::de::Error>(self, v: f64) -> Result<Self::Value, E> {
                Number::from_f64(v)
                    .map(|n| UniqueValue(Value::Number(n)))
                    .ok_or_else(|| E::custom("Nonfinite number"))
            }
            fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_string<E: serde::de::Error>(self, v: String) -> Result<Self::Value, E> {
                Ok(UniqueValue(v.into()))
            }
            fn visit_seq<A: SeqAccess<'de>>(self, mut a: A) -> Result<Self::Value, A::Error> {
                let mut values = Vec::new();
                while let Some(value) = a.next_element::<UniqueValue>()? {
                    values.push(value.0);
                }
                Ok(UniqueValue(Value::Array(values)))
            }
            fn visit_map<A: MapAccess<'de>>(self, mut a: A) -> Result<Self::Value, A::Error> {
                let mut values = Map::new();
                while let Some((key, value)) = a.next_entry::<String, UniqueValue>()? {
                    if values.insert(key.clone(), value.0).is_some() {
                        return Err(serde::de::Error::custom(format!(
                            "Duplicate response key: {key}"
                        )));
                    }
                }
                Ok(UniqueValue(Value::Object(values)))
            }
        }
        d.deserialize_any(V)
    }
}
