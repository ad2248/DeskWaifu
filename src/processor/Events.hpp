#pragma once

namespace ProcessorEvents {
constexpr auto ReadingClip = "reading.clip";
constexpr auto ReadImage = "image.read";
constexpr auto ImageReady = "image.ready";
constexpr auto Screenshot = "screenshot.capture";
constexpr auto ScreenshotReady = "screenshot.ready";
constexpr auto Recognize = "ocr.recognize";
constexpr auto Recognized = "ocr.completed";
constexpr auto Speak = "speech.speak";
constexpr auto Spoken = "speech.completed";
constexpr auto Cancel = "operation.cancel";
constexpr auto Failed = "operation.failed";
constexpr auto ReadingState = "reading.state";
constexpr auto IpcReady = "ipc.ready";
}
