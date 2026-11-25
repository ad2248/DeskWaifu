//! 定时录音、背压及录音槽生命周期。
use deskwaifu_sdk::{AudioStream, OsError};
use futures_core::Stream;
use std::{
    cell::RefCell,
    future::Future,
    pin::Pin,
    rc::Rc,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    task::{Context, Poll},
    time::Duration,
};
use tokio::sync::{Semaphore, mpsc, oneshot};

#[cfg(target_os = "linux")]
mod pipewire;

const QUEUE_BLOCKS: usize = 32;

pub(crate) struct AudioInput {
    slot: Arc<Semaphore>,
    stopping: Arc<AtomicBool>,
}

impl Default for AudioInput {
    fn default() -> Self {
        Self {
            slot: Arc::new(Semaphore::new(1)),
            stopping: Arc::new(AtomicBool::new(false)),
        }
    }
}

impl AudioInput {
    pub(crate) async fn capture(&self, duration: Duration) -> Result<AudioStream, OsError> {
        #[cfg(target_os = "linux")]
        return self.capture_with(duration, pipewire::record).await;
        #[cfg(not(target_os = "linux"))]
        {
            let _ = duration;
            Err(OsError::Unavailable)
        }
    }

    async fn capture_with(
        &self,
        duration: Duration,
        record: impl FnOnce(Rc<RefCell<Recording>>) -> Result<(), OsError> + Send + 'static,
    ) -> Result<AudioStream, OsError> {
        if duration.is_zero() || duration > Duration::from_secs(300) {
            return Err(OsError::InvalidDuration);
        }
        if self.stopping.load(Ordering::Acquire) {
            return Err(OsError::Unavailable);
        }
        let permit = self
            .slot
            .clone()
            .try_acquire_owned()
            .map_err(|error| match error {
                tokio::sync::TryAcquireError::Closed => OsError::Unavailable,
                tokio::sync::TryAcquireError::NoPermits => OsError::Busy,
            })?;
        let (start_tx, start_rx) = oneshot::channel();
        let (end_tx, end_rx) = oneshot::channel();
        let (chunks_tx, chunks_rx) = mpsc::channel(QUEUE_BLOCKS);
        let recording = Recording {
            duration,
            start: Some(start_tx),
            chunks: chunks_tx,
            stopping: self.stopping.clone(),
        };
        tokio::task::spawn_blocking(move || {
            let recording = Rc::new(RefCell::new(recording));
            let result = record(recording.clone());
            let start = recording.borrow_mut().start.take();
            drop(recording);
            // 设备已释放，随后才报告操作结束。
            drop(permit);
            if let Some(start) = start {
                let _ = start.send(Err(result.err().unwrap_or(OsError::Backend)));
            }
            let _ = end_tx.send(result);
        });
        // Future 在启动期间丢弃时也会关闭 chunks_rx，通知后台结束。
        start_rx.await.map_err(|_| OsError::Backend)??;
        Ok(Box::pin(PcmStream {
            chunks: chunks_rx,
            end: Some(end_rx),
        }))
    }

    pub(crate) async fn shutdown(&self) {
        self.stopping.store(true, Ordering::Release);
        if let Ok(permit) = self.slot.acquire().await {
            self.slot.close();
            drop(permit);
        }
    }
}

struct Recording {
    duration: Duration,
    start: Option<oneshot::Sender<Result<(), OsError>>>,
    chunks: mpsc::Sender<Vec<u8>>,
    stopping: Arc<AtomicBool>,
}

impl Recording {
    fn cancelled(&self) -> bool {
        self.chunks.is_closed() || self.stopping.load(Ordering::Acquire)
    }

    fn started(&mut self) {
        if let Some(start) = self.start.take() {
            let _ = start.send(Ok(()));
        }
    }

    fn push(&self, pcm: &[u8]) -> Result<(), OsError> {
        if pcm.is_empty() {
            return Ok(());
        }
        let permit = self.chunks.try_reserve().map_err(|error| match error {
            mpsc::error::TrySendError::Full(_) => OsError::Overflow,
            mpsc::error::TrySendError::Closed(_) => OsError::Unavailable,
        })?;
        permit.send(pcm.to_vec());
        Ok(())
    }
}

struct PcmStream {
    chunks: mpsc::Receiver<Vec<u8>>,
    end: Option<oneshot::Receiver<Result<(), OsError>>>,
}

impl Stream for PcmStream {
    type Item = Result<Vec<u8>, OsError>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        match self.chunks.poll_recv(cx) {
            Poll::Ready(Some(chunk)) => return Poll::Ready(Some(Ok(chunk))),
            Poll::Pending => return Poll::Pending,
            Poll::Ready(None) => {}
        }
        let Some(end) = &mut self.end else {
            return Poll::Ready(None);
        };
        let result = std::task::ready!(Pin::new(end).poll(cx)).unwrap_or(Err(OsError::Backend));
        self.end = None;
        Poll::Ready(result.err().map(Err))
    }
}
