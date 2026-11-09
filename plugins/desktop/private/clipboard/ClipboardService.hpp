#pragma once

#include "event/EventEndpoint.hpp"
#include <QObject>
#include <QProcess>
#include <QTimer>

// 所有方法在端点所属线程调用。program 仅用于测试注入；生产使用默认 wl-copy。
class ClipboardService final : public QObject
{
public:
    explicit ClipboardService(EventEndpoint& endpoint, QString program = QStringLiteral("wl-copy"));
    ~ClipboardService() override;
    void Deliver(const EventDelivery& delivery);
    void Stop() noexcept;
private:
    void Reply(RequestId id, bool external, const QString& token, bool hasToken,
               bool ok, const QString& code, const QString& error, const QString& state);
    void Complete(bool ok, const QString& code, const QString& error, const QString& state);
    EventEndpoint& _endpoint;
    QString _program, _token;
    QProcess* _process = nullptr;
    QTimer _deadline;
    RequestId _request = 0;
    bool _external = false, _hasToken = false, _stopped = false;
};
