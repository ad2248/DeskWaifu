#pragma once

#include "IScreenshotService.hpp"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QTimer>
#include <QVariantMap>

class PortalScreenshotService final : public IScreenshotService
{
    Q_OBJECT
public:
    explicit PortalScreenshotService(QObject* parent = nullptr);
    // A separate bus can be supplied for isolated tests; normal callers use the factory.
    PortalScreenshotService(QDBusConnection bus, int timeoutMs, QObject* parent = nullptr);
    ~PortalScreenshotService() override;
    QString Capture(const QString& parentWindow = {}) override;
    bool Cancel(const QString& requestId) override;
    bool Busy() const override { return bool(_request) || _decoding; }

private slots:
    void OnResponse(uint response, const QVariantMap& results, const QDBusMessage& message);

private:
    struct Request;
    bool Subscribe(const std::shared_ptr<Request>& request, const QString& path);
    void Unsubscribe(const std::shared_ptr<Request>& request);
    void Fail(const std::shared_ptr<Request>& request, Error error, const QString& message);
    void Decode(const std::shared_ptr<Request>& request, const QString& path);
    QDBusConnection _bus;
    QTimer _deadline;
    std::shared_ptr<Request> _request;
    bool _decoding = false;
};
