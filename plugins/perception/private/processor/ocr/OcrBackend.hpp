#pragma once

#include <QJsonObject>
#include <QString>
#include <QtPlugin>

// Private, versioned interface. The optional module and host must use matching Qt/C++ ABIs.
class OcrBackend
{
public:
    virtual ~OcrBackend() = default;
    virtual QJsonObject Initialize(const QString& detectionDirectory, const QString& recognitionDirectory) = 0;
    virtual QJsonObject Recognize(const QString& image) = 0;
    virtual void Release() noexcept = 0;
};

class OcrBackendFactory
{
public:
    virtual ~OcrBackendFactory() = default;
    virtual OcrBackend* Create() = 0;
};

#define DeskWaifuOcrBackend_iid "org.deskwaifu.OcrBackendFactory/1.0"
Q_DECLARE_INTERFACE(OcrBackendFactory, DeskWaifuOcrBackend_iid)
