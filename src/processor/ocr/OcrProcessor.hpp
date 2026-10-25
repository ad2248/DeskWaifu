#pragma once

#include "processor/Processor.hpp"
#include "contracts/ImageFile.hpp"
#include "OcrClient.hpp"

class OcrProcessor final : public Processor
{
public:
    explicit OcrProcessor(ProcessorEventLoop& loop);
    OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client);
    ~OcrProcessor() override;
    void Stop();
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    IProcessor* _requester = nullptr;
    ImageFilePtr _image, _retiringImage;
    QString _transportRequest;
    std::unique_ptr<OcrClient> _client; // 先销毁并回收工作进程，再释放临时图像。
};
