#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <memory>

// 经用户授权的单次截图；接口不涉及 OCR、存储或桌面坐标。
class IScreenshotService : public QObject
{
    Q_OBJECT
public:
    enum class Error { Cancelled, Unavailable, BackendFailure, InvalidResponse, ImageReadFailed, TimedOut };
    Q_ENUM(Error)

    explicit IScreenshotService(QObject* parent = nullptr) : QObject(parent) {}
    ~IScreenshotService() override = default;

    // 返回不透明请求 ID；忙碌时返回空字符串。服务存活期间，已接受的请求
    // 会异步发出且仅发出一次完成信号。
    // parentWindow 为可选的平台窗口标识，留空表示无父窗口。
    virtual QString Capture(const QString& parentWindow = {}) = 0;
    // 取消当前匹配请求，成功返回 true 并以 Failed 通知；无匹配请求返回 false。
    virtual bool Cancel(const QString& requestId) = 0;
    // 存在授权请求或尚未结束的后台解码时返回 true。
    virtual bool Busy() const = 0;

signals:
    // 返回已解码图像，接收方可自行决定存储和后续处理。
    void Captured(const QString& requestId, const QImage& image);
    void Failed(const QString& requestId, IScreenshotService::Error error, const QString& message);
};

Q_DECLARE_METATYPE(IScreenshotService::Error)

// 应用只依赖此接口，不感知 Portal 或合成器的具体实现。
std::unique_ptr<IScreenshotService> CreateScreenshotService();
