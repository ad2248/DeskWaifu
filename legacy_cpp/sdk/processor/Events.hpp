// 汇集处理器之间约定的领域事件与能力名称。
#pragma once

#include "contracts/SpeechEvents.hpp"

// 图片读取、OCR、语音与阅读编排共享的事件名及能力名。
// 具体负载由对应插件约定，宿主仅按名称路由。
namespace ProcessorEvents {
constexpr auto ReadingClip = "reading.clip";
constexpr auto ReadImage = "image.read";
constexpr auto ImageReady = "image.ready";
constexpr auto Screenshot = "screenshot.capture";
constexpr auto ScreenshotReady = "screenshot.ready";
constexpr auto Recognize = "ocr.recognize";
constexpr auto Recognized = "ocr.completed";
constexpr auto Speak = SpeechEvents::Speak;
constexpr auto Spoken = SpeechEvents::Completed;
constexpr auto Cancel = "operation.cancel";
constexpr auto Failed = "operation.failed";
constexpr auto ReadingState = "reading.state";
constexpr auto IpcReady = "ipc.ready";
}
