#pragma once

#include "processor/Processor.hpp"
#include "processor/image/ImageFile.hpp"
#include "OcrClient.hpp"

class OcrProcessor final : public Processor
{
public:
    explicit OcrProcessor(ProcessorEventLoop& loop);
    OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client);
    ~OcrProcessor() override;
    bool HandleEvent(const ProcessorEvent& event) override;
    OcrClient& Client() { return *_client; } // 适配现有外部 OCR API 的兼容入口。
private:
    IProcessor* _requester = nullptr;
    ImageFilePtr _image, _retiringImage;
    QString _transportRequest;
    std::unique_ptr<OcrClient> _client; // 先销毁并回收工作进程，再释放临时图像。
};
