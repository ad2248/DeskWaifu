//! 事件驱动的单步服务；fd 注册和协议状态都由 Server 持有。
use super::*;
use std::{future::poll_fn, task::Poll};
use tokio::io::{Interest, unix::AsyncFd};

#[cfg(test)]
mod tests;

impl Server {
    /// 等待连接、读写就绪或活动超时，完成一轮有限处理后返回。
    ///
    /// 必须在启用 I/O 和 time 的 Tokio runtime 中调用。每轮最多接受 32 个
    /// 连接，每个客户端最多读取 4 KiB、执行一个请求、写入 4 KiB。
    /// 回调在调用线程执行，不要求 Send。适合与 runtime 截止时间及退出信号
    /// 一起 select；取消此 future 不丢失连接、半帧或待发送响应。
    // Tokio 1.54 的安全构造器已弃用，但替代接口要求 unsafe（本工作区禁止）。
    // 此处只注册拥有所有权的 std socket 副本，且从不替换/关闭注册内的 fd。
    #[allow(deprecated)]
    pub async fn poll_ready(
        &mut self,
        mut handler: impl FnMut(&[String]) -> Result<String, String>,
    ) -> io::Result<()> {
        if self.readiness.is_none() {
            self.readiness = Some(AsyncFd::new(self.listener.try_clone()?)?);
        }
        for client in &mut self.clients {
            if client.readiness.is_none() {
                client.readiness = Some(AsyncFd::new(client.stream.try_clone()?)?);
            }
        }

        let deadline = self.clients.iter().map(|c| c.active + CLIENT_TIMEOUT).min();
        let timeout = async {
            match deadline {
                Some(deadline) => tokio::time::sleep_until(deadline).await,
                None => std::future::pending().await,
            }
        };
        tokio::select! {
            result = poll_fn(|cx| {
                let mut ready = false;
                if self.clients.len() < MAX_CLIENTS {
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
                        Poll::Ready(Err(error)) => return Poll::Ready(Err(error)),
                        Poll::Pending => {}
                    }
                }
                if ready { Poll::Ready(Ok(())) } else { Poll::Pending }
            }) => result?,
            () = timeout => {}
        }

        // 从此处到返回没有 await：每次有副作用的操作都立即保存到 Server。
        for _ in self.clients.len()..MAX_CLIENTS {
            match self
                .readiness
                .as_ref()
                .unwrap()
                .try_io(Interest::READABLE, |listener| listener.accept())
            {
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
