#pragma once

#include "SpeechProcessor.hpp"

// Plugin-private bridge onto the existing speech state machine. It registers no
// capabilities itself: exactly one plugin endpoint provides speech.speak/v1.
// 将能力请求 ID 与语音事件关联；借用的端点须覆盖此桥接器的生命周期。
// 所有入口在总线线程调用，以保持请求接受与取消的顺序。
class SpeechEndpoint final : public Processor {
public:
    SpeechEndpoint(ProcessorEventLoop& loop, EventEndpoint& endpoint, const QString& apiKey);
    // 注入的 player 为借用对象，须比此桥接器存活更久。
    SpeechEndpoint(ProcessorEventLoop& loop, EventEndpoint& endpoint, SpeechPlayer& player);
    ~SpeechEndpoint() override;
    // 立即交给语音状态机接受，不再次排队；非零 request 的终态通过端点回复。
    void Speak(RequestId request, const QVariantMap& data);
    // 仅取消匹配请求；request 为 0 时不执行操作。
    void Cancel(RequestId request);
    // 更新语音来源供后续连接使用的凭据。
    void SetApiKey(const QString& key);
    // 停止语音处理器，不失效借用的插件端点。
    void Stop() noexcept override;
    // 将内部语音完成或失败事件转换为能力回复；消费时返回 true。
    bool HandleEvent(const ProcessorEvent& event) override;

private:
    EventEndpoint& _endpoint;
    SpeechProcessor _speech;
};
