#pragma once

#include "OcrClient.hpp"
#include "contracts/ImageFile.hpp"
#include "processor/Processor.hpp"

// 将 OCR 命令及识别能力请求适配为单任务客户端调用，并保活临时图像。
class OcrProcessor final : public Processor {
public:
    explicit OcrProcessor(ProcessorEventLoop& loop);
    explicit OcrProcessor(PluginContext& context);
    // 接管 client 所有权；客户端应属于当前事件循环线程。
    OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client);
    ~OcrProcessor() override;
    // 失效端点、等待客户端后台结束，再释放其借用的临时图像。
    void Stop() noexcept override;
    // 消费 OCR 命令、识别请求或匹配的取消事件时返回 true。
    bool HandleEvent(const ProcessorEvent& event) override;

private:
    OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client, PluginContext* context);
    bool Recognize(const ProcessorEvent& event, RequestId request);
    void Complete(const QString& type, IProcessorPlugin* receiver, RequestId request, QVariantMap data = { });
    std::unique_ptr<EventEndpoint> _ownedEndpoint;
    EventEndpoint* _endpoint = nullptr;
    RequestId _request = 0;
    IProcessorPlugin* _requester = nullptr;
    ImageFilePtr _image, _retiringImage;
    QString _transportRequest;
    std::unique_ptr<OcrClient> _client; // 先销毁并等待工作线程结束，再释放临时图像。
};
