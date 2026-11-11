#pragma once

#include "contracts/ImageFile.hpp"
#include "processor/Processor.hpp"

// 协调剪贴板图像读取、OCR 与语音播报；再次触发会取消当前流水线。
// 在事件循环线程串行推进，并持有临时图像直到识别步骤结束。
class ReadingClipProcessor final : public Processor {
public:
    enum class State { Idle, WaitingImage, WaitingOcr, WaitingSpeech, Cancelling };
    // 借用三个已注册处理器，供直接事件派发使用；依赖须保持存活。
    ReadingClipProcessor(ProcessorEventLoop& loop, IProcessorPlugin& images, IProcessorPlugin& ocr, IProcessorPlugin& speech);
    // 借用插件端点，由调用方转发命令交付并管理端点生命周期。
    ReadingClipProcessor(ProcessorEventLoop& loop, EventEndpoint& endpoint);
    ~ReadingClipProcessor() override;
    // 幂等取消当前步骤并释放图像；仅失效自身持有的端点。
    void Stop() noexcept override;
    // 响应朗读及状态能力请求；朗读操作入队后即回复 queued，不等待播报结束。
    void HandleDelivery(const EventDelivery& delivery);
    // 消费触发、命令或当前步骤结果时返回 true；不接收无关发送者的结果。
    bool HandleEvent(const ProcessorEvent& event) override;
    State CurrentState() const { return _state; }
    QString LastError() const { return _error; }
    // 返回含 ok、协议状态名和最近错误的快照；不发起任何异步操作。
    QVariantMap Status() const;

private:
    void SetState(State state);
    void Finish(QString error = { });
    void Step(State state, const QString& type, const QString& capability, IProcessorPlugin* legacy, QVariantMap data = { });
    bool Result(const QString& type, const QVariantMap& data);
    IProcessorPlugin* _images = nullptr;
    IProcessorPlugin* _ocr = nullptr;
    IProcessorPlugin* _speech = nullptr;
    std::unique_ptr<EventEndpoint> _ownedEndpoint;
    EventEndpoint* _endpoint = nullptr;
    RequestId _request = 0;
    IProcessorPlugin* _waitingFor = nullptr;
    State _state = State::Idle;
    ImageFilePtr _image;
    QString _error;
    bool _stopped = false;
};
