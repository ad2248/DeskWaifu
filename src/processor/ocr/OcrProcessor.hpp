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
    OcrClient& Client() { return *_client; } // Compatibility adapter for the existing external OCR API.
private:
    IProcessor* _requester = nullptr;
    ImageFilePtr _image, _retiringImage;
    QString _transportRequest;
    std::unique_ptr<OcrClient> _client; // Destroy/reap the worker before releasing temporary images.
};
