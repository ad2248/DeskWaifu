//! 平台私有 JSON IPC；不依赖插件 SDK 或 runtime。
#![cfg(unix)]

use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::{self, BufRead, BufReader, Read, Write},
    os::unix::{
        fs::{MetadataExt, PermissionsExt},
        net::{UnixListener, UnixStream},
    },
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

/// 主程序与命令行客户端共用的固定 IPC 地址。
pub const SOCKET_PATH: &str = "/tmp/deskwaifu.socket";

/// 每条 JSON 的最大字节数（不含末尾 LF），请求和响应使用相同限制。
pub const MAX_FRAME_BYTES: usize = 64 * 1024;
pub const MAX_CLIENTS: usize = 32;
pub const CLIENT_TIMEOUT: Duration = Duration::from_secs(10);

#[cfg(test)]
mod tests;

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
    pub source: String,
    pub payload: Vec<String>,
}

impl Request {
    pub fn cli(payload: Vec<String>) -> Self {
        Self {
            source: "cli".into(),
            payload,
        }
    }
}

#[derive(Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "status", rename_all = "snake_case", deny_unknown_fields)]
pub enum Response {
    Ok { output: String },
    Error { error: String },
}

impl Response {
    fn error(error: impl Into<String>) -> Self {
        Self::Error {
            error: error.into(),
        }
    }
}

fn encode(response: &Response) -> Vec<u8> {
    let mut bytes = serde_json::to_vec(response).expect("string response is serializable");
    if bytes.len() > MAX_FRAME_BYTES {
        bytes = serde_json::to_vec(&Response::error("response exceeds 64 KiB")).unwrap();
    }
    bytes.push(b'\n');
    bytes
}

fn dispatch(
    frame: &[u8],
    handler: &mut impl FnMut(&[String]) -> Result<String, String>,
) -> Response {
    // serde_json 校验 UTF-8、信封和字符串数组；正文不再解析。
    match serde_json::from_slice::<Request>(frame) {
        Ok(request) if request.source == "cli" => match handler(&request.payload) {
            Ok(output) => Response::Ok { output },
            Err(error) => Response::error(error),
        },
        Ok(_) => Response::error("unsupported source (only cli is supported)"),
        Err(error) => Response::error(format!("invalid request: {error}")),
    }
}

struct Client {
    stream: UnixStream,
    input: Vec<u8>,
    output: Vec<u8>,
    written: usize,
    eof: bool,
    closing: bool,
    active: Instant,
}

impl Client {
    fn new(stream: UnixStream) -> io::Result<Self> {
        stream.set_nonblocking(true)?;
        Ok(Self {
            stream,
            input: Vec::new(),
            output: Vec::new(),
            written: 0,
            eof: false,
            closing: false,
            active: Instant::now(),
        })
    }

    // 每轮最多读取 4 KiB、执行一个请求、写入 4 KiB，避免单连接饿死其他客户端。
    fn poll(
        &mut self,
        handler: &mut impl FnMut(&[String]) -> Result<String, String>,
    ) -> io::Result<bool> {
        if self.active.elapsed() >= CLIENT_TIMEOUT {
            return Ok(false);
        }
        if self.output.is_empty() && !self.closing {
            if !self.input.contains(&b'\n') && !self.eof {
                let mut chunk = [0; 4096];
                let limit = chunk.len().min(MAX_FRAME_BYTES + 1 - self.input.len());
                match self.stream.read(&mut chunk[..limit]) {
                    Ok(0) => self.eof = true,
                    Ok(n) => {
                        self.input.extend_from_slice(&chunk[..n]);
                        self.active = Instant::now();
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                    Err(error) => return Err(error),
                }
            }
            if let Some(end) = self.input.iter().position(|byte| *byte == b'\n') {
                self.output = encode(&dispatch(&self.input[..end], handler));
                self.input.drain(..=end);
            } else if self.input.len() > MAX_FRAME_BYTES {
                self.output = encode(&Response::error("request exceeds 64 KiB"));
                self.closing = true;
            } else if self.eof {
                if self.input.is_empty() {
                    return Ok(false);
                }
                self.output = encode(&Response::error("incomplete request (expected LF)"));
                self.closing = true;
            }
        }
        if !self.output.is_empty() {
            let end = (self.written + 4096).min(self.output.len());
            match self.stream.write(&self.output[self.written..end]) {
                Ok(0) => return Ok(false),
                Ok(n) => {
                    self.written += n;
                    self.active = Instant::now();
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(error) => return Err(error),
            }
            if self.written == self.output.len() {
                self.output.clear();
                self.written = 0;
            }
        }
        Ok(!self.closing || !self.output.is_empty())
    }
}

/// 非阻塞 socket 监听器，由平台主循环调用 poll；命令回调仍在 runtime 线程运行。
/// 不覆盖已有路径（包括失效 socket），Drop 只清理自己绑定的 inode。
pub struct Server {
    listener: UnixListener,
    path: PathBuf,
    identity: (u64, u64),
    clients: Vec<Client>,
}

impl Server {
    pub fn bind(path: impl AsRef<Path>) -> io::Result<Self> {
        let path = path.as_ref().to_path_buf();
        let listener = UnixListener::bind(&path)?;
        let metadata = fs::symlink_metadata(&path)?;
        let server = Self {
            listener,
            path,
            identity: (metadata.dev(), metadata.ino()),
            clients: Vec::new(),
        };
        fs::set_permissions(&server.path, fs::Permissions::from_mode(0o600))?;
        server.listener.set_nonblocking(true)?;
        Ok(server)
    }

    pub fn poll(
        &mut self,
        mut handler: impl FnMut(&[String]) -> Result<String, String>,
    ) -> io::Result<()> {
        // 超过连接上限时不继续 accept；内核 backlog 提供背压。
        for _ in 0..MAX_CLIENTS {
            if self.clients.len() >= MAX_CLIENTS {
                break;
            }
            match self.listener.accept() {
                Ok((stream, _)) => {
                    if let Ok(client) = Client::new(stream) {
                        self.clients.push(client);
                    }
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(error),
            }
        }
        self.clients
            .retain_mut(|client| client.poll(&mut handler).unwrap_or(false));
        Ok(())
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        if fs::symlink_metadata(&self.path)
            .is_ok_and(|meta| (meta.dev(), meta.ino()) == self.identity)
        {
            let _ = fs::remove_file(&self.path);
        }
    }
}

/// CLI 每次调用建立一个连接，发送一条请求并等待一条响应。
/// 读写分别使用 CLIENT_TIMEOUT；这不是命令执行截止时间。
pub fn call(path: impl AsRef<Path>, payload: Vec<String>) -> io::Result<Response> {
    let mut frame = serde_json::to_vec(&Request::cli(payload))?;
    if frame.len() > MAX_FRAME_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "request exceeds 64 KiB",
        ));
    }
    frame.push(b'\n');
    let mut stream = UnixStream::connect(path)?;
    stream.set_read_timeout(Some(CLIENT_TIMEOUT))?;
    stream.set_write_timeout(Some(CLIENT_TIMEOUT))?;
    stream.write_all(&frame)?;
    let mut response = Vec::new();
    BufReader::new(stream)
        .take((MAX_FRAME_BYTES + 2) as u64)
        .read_until(b'\n', &mut response)?;
    if response.last() != Some(&b'\n') || response.len() > MAX_FRAME_BYTES + 1 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid or oversized IPC response",
        ));
    }
    serde_json::from_slice(&response)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))
}
