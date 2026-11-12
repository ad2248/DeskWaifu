//! Bounded newline-delimited Unix socket transport.
use mio::net::{UnixListener, UnixStream};
use mio::{Events, Interest, Poll, Token};
use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::path::Path;
use std::time::Duration;

const LISTENER: Token = Token(0);
pub const MAX_REQUEST: usize = 64 * 1024;
pub const MAX_RESPONSE: usize = 8 * 1024 * 1024;

struct Client {
    stream: UnixStream,
    input: Vec<u8>,
    output: Option<Vec<u8>>,
    sent: usize,
    eof: bool,
}

pub struct IpcServer {
    poll: Poll,
    listener: UnixListener,
    clients: HashMap<Token, Client>,
    next: usize,
    rotation: usize,
    accept_pending: bool,
}

impl IpcServer {
    pub fn bind(path: impl AsRef<Path>) -> io::Result<Self> {
        let poll = Poll::new()?;
        let mut listener = UnixListener::bind(path)?;
        poll.registry()
            .register(&mut listener, LISTENER, Interest::READABLE)?;
        Ok(Self {
            poll,
            listener,
            clients: HashMap::new(),
            next: 1,
            rotation: 0,
            accept_pending: false,
        })
    }

    /// Poll once; `handler` receives one complete JSON line excluding LF and
    /// returns a response line excluding LF. Transport work is bounded per poll.
    pub fn poll_once<F>(&mut self, timeout: Option<Duration>, mut handler: F) -> io::Result<usize>
    where
        F: FnMut(&[u8]) -> Vec<u8>,
    {
        let mut events = Events::with_capacity(128);
        let timeout = if self.accept_pending {
            Some(Duration::ZERO)
        } else {
            timeout
        };
        self.poll.poll(&mut events, timeout)?;
        // Keep accepting even without a fresh readiness edge: the four-accept
        // budget can leave connections queued on edge-triggered pollers.
        self.accept_pending = true;
        for _ in 0..4 {
            match self.listener.accept() {
                Ok((mut stream, _)) => {
                    let token = Token(self.next);
                    self.next += 1;
                    self.poll
                        .registry()
                        .register(&mut stream, token, Interest::READABLE)?;
                    self.clients.insert(
                        token,
                        Client {
                            stream,
                            input: Vec::new(),
                            output: None,
                            sent: 0,
                            eof: false,
                        },
                    );
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                    self.accept_pending = false;
                    break;
                }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(error),
            }
        }

        let mut order: Vec<_> = self.clients.keys().copied().collect();
        order.sort_by_key(|token| token.0);
        if !order.is_empty() {
            let start = self.rotation % order.len();
            order.rotate_left(start);
            self.rotation = (start + 1) % order.len();
        }

        let mut dispatched = 0;
        let mut remove = Vec::new();
        for token in order {
            let client = self
                .clients
                .get_mut(&token)
                .expect("client token came from map");
            let mut read_budget = 4096usize;

            while client.output.is_none() && !client.eof && read_budget > 0 {
                if dispatched >= 16 {
                    break;
                }
                if let Some(newline) = client.input.iter().position(|byte| *byte == b'\n') {
                    if newline > MAX_REQUEST {
                        remove.push(token);
                        break;
                    }
                    let line = client.input.drain(..=newline).collect::<Vec<_>>();
                    let response = handler(&line[..newline]);
                    if response.len() > MAX_RESPONSE || response.contains(&b'\n') {
                        remove.push(token);
                        break;
                    }
                    let mut framed = response;
                    framed.push(b'\n');
                    client.output = Some(framed);
                    client.sent = 0;
                    dispatched += 1;
                    break;
                }
                if client.input.len() > MAX_REQUEST {
                    remove.push(token);
                    break;
                }

                let mut buffer = [0u8; 4096];
                let count = buffer.len().min(read_budget);
                match client.stream.read(&mut buffer[..count]) {
                    Ok(0) => {
                        client.eof = true;
                        break;
                    }
                    Ok(read) => {
                        read_budget -= read;
                        client.input.extend_from_slice(&buffer[..read]);
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => {
                        read_budget = read_budget.saturating_sub(1);
                    }
                    Err(_) => {
                        remove.push(token);
                        break;
                    }
                }
            }

            if let Some(output) = client.output.as_ref() {
                let mut write_budget = 64 * 1024usize;
                let mut attempts = 0usize;
                while client.sent < output.len() && write_budget > 0 && attempts < 16 {
                    attempts += 1;
                    let end = output.len().min(client.sent + write_budget);
                    match client.stream.write(&output[client.sent..end]) {
                        Ok(0) => break,
                        Ok(written) => {
                            client.sent += written;
                            write_budget -= written;
                        }
                        Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                        Err(error) if error.kind() == io::ErrorKind::Interrupted => {
                            write_budget = write_budget.saturating_sub(1);
                        }
                        Err(_) => {
                            remove.push(token);
                            break;
                        }
                    }
                }
                if client.sent == output.len() {
                    client.output = None;
                    client.sent = 0;
                }
            }

            if !remove.contains(&token) {
                let interest = if client.output.is_some() {
                    Interest::WRITABLE
                } else {
                    Interest::READABLE
                };
                self.poll
                    .registry()
                    .reregister(&mut client.stream, token, interest)?;
                if client.eof && client.output.is_none() {
                    remove.push(token);
                }
            }
        }

        remove.sort_by_key(|token| token.0);
        remove.dedup();
        for token in remove {
            self.clients.remove(&token);
        }
        Ok(dispatched)
    }
}
