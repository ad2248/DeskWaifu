//! 插件可持有的异步系统服务接口。
use futures_core::Stream;
use std::{future::Future, pin::Pin, time::Duration};

pub type LocalFuture<'a, T> = Pin<Box<dyn Future<Output = T> + 'a>>;
pub type AudioStream = Pin<Box<dyn Stream<Item = Result<Vec<u8>, OsError>>>>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OsError {
    Unavailable,
    Busy,
    InvalidDuration,
    Timeout,
    TooLarge,
    InvalidText,
    Backend,
    Overflow,
}

impl std::fmt::Display for OsError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Unavailable => "system service is unavailable",
            Self::Busy => "system service is busy",
            Self::InvalidDuration => "invalid recording duration",
            Self::Timeout => "system operation timed out",
            Self::TooLarge => "clipboard text exceeds 1 MiB",
            Self::InvalidText => "clipboard text is not valid UTF-8",
            Self::Backend => "system backend failed",
            Self::Overflow => "audio input queue overflowed",
        })
    }
}

impl std::error::Error for OsError {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClipboardWriteError {
    /// 确认本次写入未生效。
    Failed(OsError),
    /// 无法确认本次写入是否生效。
    Unknown(OsError),
}

impl std::fmt::Display for ClipboardWriteError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Failed(error) => write!(f, "clipboard write failed: {error}"),
            Self::Unknown(error) => write!(f, "clipboard write result unknown: {error}"),
        }
    }
}

impl std::error::Error for ClipboardWriteError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Failed(error) | Self::Unknown(error) => Some(error),
        }
    }
}

pub trait OsServices {
    /// 设备启动后返回流。每块含有完整的 16 kHz 单声道 S16LE 样本。
    /// 启动失败由 Future 返回，采集中出错由流的最后一项返回。
    /// 丢弃启动 Future 或流会请求后台停止采集并清理设备。
    fn capture_audio(&self, duration: Duration) -> LocalFuture<'_, Result<AudioStream, OsError>>;
    /// `None` 表示没有文本，空字符串也是有效文本。
    fn read_clipboard_text(&self) -> LocalFuture<'_, Result<Option<String>, OsError>>;
    /// 取消等待时，已提交的写入仍可能生效。
    fn write_clipboard_text(
        &self,
        text: String,
    ) -> LocalFuture<'_, Result<(), ClipboardWriteError>>;
}

pub(crate) struct Unavailable;

impl OsServices for Unavailable {
    fn capture_audio(&self, _: Duration) -> LocalFuture<'_, Result<AudioStream, OsError>> {
        Box::pin(async { Err(OsError::Unavailable) })
    }

    fn read_clipboard_text(&self) -> LocalFuture<'_, Result<Option<String>, OsError>> {
        Box::pin(async { Err(OsError::Unavailable) })
    }

    fn write_clipboard_text(&self, _: String) -> LocalFuture<'_, Result<(), ClipboardWriteError>> {
        Box::pin(async { Err(ClipboardWriteError::Failed(OsError::Unavailable)) })
    }
}
