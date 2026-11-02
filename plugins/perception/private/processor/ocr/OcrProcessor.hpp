#pragma once

#include "processor/Processor.hpp"
#include "contracts/ImageFile.hpp"
#include "OcrClient.hpp"

class OcrProcessor final : public Processor
{
public:
    explicit OcrProcessor(ProcessorEventLoop& loop);
    explicit OcrProcessor(PluginContext& context);
    OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client);
    ~OcrProcessor() override;
    void Stop() noexcept override;
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client, PluginContext* context);
    bool Recognize(const ProcessorEvent& event, RequestId request);
    void Complete(const QString& type, IProcessorPlugin* receiver, RequestId request, QVariantMap data = {});
    std::unique_ptr<EventEndpoint> _ownedEndpoint;
    EventEndpoint* _endpoint = nullptr;
    RequestId _request = 0;
    IProcessorPlugin* _requester = nullptr;
    ImageFilePtr _image, _retiringImage;
    QString _transportRequest;
    std::unique_ptr<OcrClient> _client; // 先销毁并回收工作进程，再释放临时图像。
};
