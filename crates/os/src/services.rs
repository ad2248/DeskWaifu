use crate::{audio_input::AudioInput, clipboard::Clipboard};
use deskwaifu_sdk::{AudioStream, ClipboardWriteError, LocalFuture, OsError, OsServices};
use std::time::Duration;

#[derive(Default)]
pub(crate) struct PlatformServices {
    audio: AudioInput,
    clipboard: Clipboard,
}

impl PlatformServices {
    pub(crate) async fn shutdown(&self) {
        tokio::join!(self.audio.shutdown(), self.clipboard.shutdown());
    }
}

impl OsServices for PlatformServices {
    fn capture_audio(&self, duration: Duration) -> LocalFuture<'_, Result<AudioStream, OsError>> {
        Box::pin(self.audio.capture(duration))
    }

    fn read_clipboard_text(&self) -> LocalFuture<'_, Result<Option<String>, OsError>> {
        Box::pin(self.clipboard.read())
    }

    fn write_clipboard_text(
        &self,
        text: String,
    ) -> LocalFuture<'_, Result<(), ClipboardWriteError>> {
        Box::pin(self.clipboard.write(text))
    }
}
