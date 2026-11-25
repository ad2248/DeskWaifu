//! 事件驱动的单步服务；fd 注册和协议状态都由 Server 持有。
use super::*;
use std::{future::poll_fn, task::Poll};
use tokio::io::{Interest, unix::AsyncFd};

const ACCEPT_RETRY_DELAY: Duration = Duration::from_millis(100);

impl Server {
    /// 等待连接、读写就绪或活动超时，完成一轮有限处理后返回。
    ///
    /// 必须在启用 I/O 和 time 的 Tokio runtime 中调用。每轮最多接受 32 个
    /// 连接，每个客户端最多读取 4 KiB、执行一个请求、写入 4 KiB。
    /// 回调在调用线程执行，不要求 Send。适合与 runtime 截止时间及退出信号
    /// 一起 select；取消此 future 不丢失连接、半帧或待发送响应。
    /// 无法注册到事件循环的客户端会单独关闭。
    // 使用安全的 AsyncFd::new 注册拥有所有权的 std socket 副本，
    // 注册期间保持其中的 fd 不变且打开。
    #[allow(deprecated)]
    pub async fn poll_ready(
        &mut self,
        mut handler: impl FnMut(&[String]) -> Result<String, String>,
    ) -> io::Result<()> {
        self.poll_ready_connections(|_, argv| handler(argv), |_| {})
            .await
    }

    /// 异步驱动 IPC，并为每个关闭的连接调用一次 disconnect。
    #[allow(deprecated)]
    pub async fn poll_ready_connections(
        &mut self,
        handler: impl FnMut(ConnectionId, &[String]) -> Result<String, String>,
        disconnect: impl FnMut(ConnectionId),
    ) -> io::Result<()> {
        self.poll_ready_with(
            handler,
            disconnect,
            |stream| AsyncFd::new(stream.try_clone()?),
            |listener| listener.accept().map(|(stream, _)| stream),
        )
        .await
    }

    #[allow(deprecated)]
    async fn poll_ready_with(
        &mut self,
        mut handler: impl FnMut(ConnectionId, &[String]) -> Result<String, String>,
        mut disconnect: impl FnMut(ConnectionId),
        mut register_client: impl FnMut(&UnixStream) -> io::Result<AsyncFd<UnixStream>>,
        mut accept_client: impl FnMut(&UnixListener) -> io::Result<UnixStream>,
    ) -> io::Result<()> {
        if self.readiness.is_none() {
            self.readiness = Some(AsyncFd::new(self.listener.try_clone()?)?);
        }
        if self.retain_clients(
            |client| {
                if client.readiness.is_some() {
                    return true;
                }
                match register_client(&client.stream) {
                    Ok(readiness) => {
                        client.readiness = Some(readiness);
                        true
                    }
                    Err(error) => {
                        eprintln!("IPC client registration: {error}");
                        false
                    }
                }
            },
            &mut disconnect,
        ) {
            // 已处理断开事件，不再等待其他连接可读写。
            return Ok(());
        }

        self.accept_retry_at = self.accept_retry_at.filter(|retry| *retry > Instant::now());
        let deadline = self
            .clients
            .iter()
            .map(|c| c.active + CLIENT_TIMEOUT)
            .chain(self.accept_retry_at)
            .min();
        let timeout = async {
            match deadline {
                Some(deadline) => tokio::time::sleep_until(deadline).await,
                None => std::future::pending().await,
            }
        };
        let mut failed = Vec::new();
        tokio::select! {
            result = poll_fn(|cx| {
                let mut ready = false;
                if self.accept_retry_at.is_none() && self.clients.len() < MAX_CLIENTS {
                    match self.readiness.as_ref().unwrap().poll_read_ready(cx) {
                        Poll::Ready(Ok(_)) => ready = true,
                        Poll::Ready(Err(error)) => return Poll::Ready(Err(error)),
                        Poll::Pending => {}
                    }
                }
                for client in &self.clients {
                    if client.active.elapsed() >= CLIENT_TIMEOUT
                        || (client.output.is_empty()
                            && (client.eof || client.input.contains(&b'\n')
                                || client.input.len() > MAX_FRAME_BYTES))
                    {
                        ready = true;
                        continue;
                    }
                    let fd = client.readiness.as_ref().unwrap();
                    let state = if client.output.is_empty() {
                        fd.poll_read_ready(cx)
                    } else {
                        fd.poll_write_ready(cx)
                    };
                    match state {
                        Poll::Ready(Ok(_)) => ready = true,
                        Poll::Ready(Err(_)) => {
                            failed.push(client.id);
                            ready = true;
                        }
                        Poll::Pending => {}
                    }
                }
                if ready { Poll::Ready(Ok(())) } else { Poll::Pending }
            }) => result?,
            () = timeout => {}
        }

        // 从此处到返回没有 await：每次有副作用的操作都立即保存到 Server。
        for _ in self.clients.len()..MAX_CLIENTS {
            if self
                .accept_retry_at
                .is_some_and(|retry| retry > Instant::now())
            {
                break;
            }
            self.accept_retry_at = None;
            match self
                .readiness
                .as_ref()
                .unwrap()
                .try_io(Interest::READABLE, &mut accept_client)
            {
                Ok(stream) => self.add_client(stream, &mut disconnect)?,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) if matches!(error.raw_os_error(), Some(libc::EMFILE | libc::ENFILE)) => {
                    self.accept_retry_at = Some(Instant::now() + ACCEPT_RETRY_DELAY);
                    break;
                }
                Err(error) => return Err(error),
            }
        }
        self.retain_clients(
            |client| {
                !failed.contains(&client.id)
                    && (client.readiness.is_none() || client.poll(&mut handler).unwrap_or(false))
            },
            &mut disconnect,
        );
        Ok(())
    }
}
