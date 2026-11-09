#pragma once

#include "processor/Processor.hpp"
#include "SpeechPlayer.hpp"
#include <functional>
#include <optional>
#include <memory>

// 把语音请求、取消与配置事件适配为播放器操作，并向原请求者回复终态。
// 忙碌时仅接受显式替换，最多保留一个待替换请求；在事件循环线程使用。
class SpeechProcessor final : public Processor
{
public:
    // 借用 player，调用方须保证它比此处理器存活更久。
    SpeechProcessor(ProcessorEventLoop& loop, SpeechPlayer& player);
    // 创建并持有语音来源、音频输出与播放器。
    SpeechProcessor(ProcessorEventLoop& loop, const QString& apiKey);
    ~SpeechProcessor() override;
    // 幂等断开播放器回调、请求停止并释放自有资源，不再回复当前请求。
    void Stop() noexcept override;
    // 消费播报、配置或匹配请求的取消事件时返回 true。
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    void ConnectPlayer();
    void Reply(IProcessorPlugin* receiver, quint64 requestId, bool completed, const QString& error = {});
    std::unique_ptr<MiniMaxSpeechSource> _source;
    std::unique_ptr<AudioPlayer> _audio;
    std::unique_ptr<SpeechPlayer> _ownedPlayer;
    SpeechPlayer& _player;
    IProcessorPlugin* _requester = nullptr;
    quint64 _requestId = 0; // Optional correlation for plugin adapters; legacy callers omit it.
    SpeechId _speech = 0;
    bool _busy = false, _cancelled = false;
    QString _error;
    std::optional<ProcessorEvent> _replacement;
    bool _stopped = false;
};
