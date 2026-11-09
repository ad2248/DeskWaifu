// 定义语音服务请求和完成通知的共享名称。
#pragma once

// 语音提供者和编排插件共享的事件名称；宿主不解释这些领域事件。
namespace SpeechEvents {
constexpr auto Speak = "speech.speak";
constexpr auto Completed = "speech.completed";
}
