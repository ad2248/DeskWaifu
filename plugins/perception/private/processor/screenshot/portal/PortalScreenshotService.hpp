#pragma once

#include "processor/screenshot/IScreenshotService.hpp"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QTimer>
#include <QVariantMap>

// 在所属 Qt 线程通过桌面 Portal 请求交互式授权，后台解码其返回的本地文件。
// 取消不强制中止解码，解码结束前仍保持 Busy；析构会关闭未完成的 Portal 请求。
class PortalScreenshotService final : public IScreenshotService
{
    Q_OBJECT
public:
    explicit PortalScreenshotService(QObject* parent = nullptr);
    // 隔离测试可传入独立总线，普通调用方使用工厂创建。
    PortalScreenshotService(QDBusConnection bus, int timeoutMs, QObject* parent = nullptr);
    ~PortalScreenshotService() override;
    // 接受时返回请求 ID；忙碌时返回空字符串，后续结果通过完成信号给出。
    QString Capture(const QString& parentWindow = {}) override;
    // 仅取消当前匹配请求；成功返回 true，并发出 Cancelled 失败信号。
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
