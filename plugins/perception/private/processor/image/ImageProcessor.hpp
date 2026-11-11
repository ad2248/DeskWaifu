#pragma once

#include "processor/Processor.hpp"
#include <QImage>
#include <functional>

// 串行处理图像读取请求：GUI 线程取得图像，工作线程编码临时 PNG。
class ImageProcessor final : public Processor {
public:
    // clipboard 在事件循环线程调用；context 非空时由其持有端点且须保持存活。
    ImageProcessor(ProcessorEventLoop& loop, std::function<QImage()> clipboard, PluginContext* context = nullptr);
    ~ImageProcessor() override;
    // 失效端点并等待所有编码任务结束，不再发送这些任务的完成事件。
    void Stop() noexcept override;
    // 处理读取及匹配发送者的取消事件；已消费时返回 true，结果异步回复。
    bool HandleEvent(const ProcessorEvent& event) override;

private:
    bool Read(const ProcessorEvent& event, RequestId request);
    void Complete(const QString& type, IProcessorPlugin* receiver, RequestId request, QVariantMap data = { });
    std::unique_ptr<EventEndpoint> _ownedEndpoint;
    EventEndpoint* _endpoint = nullptr;
    RequestId _request = 0;
    std::function<QImage()> _clipboard;
    IProcessorPlugin* _requester = nullptr;
    bool _busy = false, _cancelled = false, _stopped = false;
};
