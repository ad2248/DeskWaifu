#pragma once

#include "IScreenshotService.hpp"
#include "processor/Processor.hpp"

// 将截图及取消事件转交授权截图服务，并向原请求者发送图像或失败事件。
// 在服务所属的 Qt 事件循环线程使用，同一时间只跟踪一次截图。
class ScreenshotProcessor final : public Processor {
public:
    ~ScreenshotProcessor() override;
    // 断开回调、取消当前请求并销毁服务；可重复调用。
    void Stop() noexcept override;
    explicit ScreenshotProcessor(ProcessorEventLoop& loop);
    // 接管 service 所有权，便于注入隔离的截图后端。
    ScreenshotProcessor(ProcessorEventLoop& loop, std::unique_ptr<IScreenshotService> service);
    // 消费截图或匹配发送者的取消事件时返回 true；忙碌时回复失败。
    bool HandleEvent(const ProcessorEvent& event) override;

private:
    std::unique_ptr<IScreenshotService> _service;
    IProcessorPlugin* _requester = nullptr;
    QString _capture;
};
