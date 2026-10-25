#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <memory>

// Single-shot, user-authorized capture. No OCR, storage or desktop coordinates in this port.
class IScreenshotService : public QObject
{
    Q_OBJECT
public:
    enum class Error { Cancelled, Unavailable, BackendFailure, InvalidResponse, ImageReadFailed, TimedOut };
    Q_ENUM(Error)

    explicit IScreenshotService(QObject* parent = nullptr) : QObject(parent) {}
    ~IScreenshotService() override = default;

    // Returns an opaque request ID, or an empty string if busy. Accepted requests complete
    // asynchronously through exactly one signal while the service remains alive.
    // parentWindow is an optional platform window identifier; empty means unparented.
    virtual QString Capture(const QString& parentWindow = {}) = 0;
    virtual bool Cancel(const QString& requestId) = 0;
    virtual bool Busy() const = 0;

signals:
    void Captured(const QString& requestId, const QImage& image);
    void Failed(const QString& requestId, IScreenshotService::Error error, const QString& message);
};

Q_DECLARE_METATYPE(IScreenshotService::Error)

// The application sees only this interface, not a portal or compositor implementation.
std::unique_ptr<IScreenshotService> CreateScreenshotService();
