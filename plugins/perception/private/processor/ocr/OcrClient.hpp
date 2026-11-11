#pragma once

#include <QJsonObject>
#include <QObject>
#include <QThread>
#include <QTimer>

// 主程序仅需 QtCore；可选模块的加载、推理和释放均在专用线程完成。
// 公共接口须在客户端所属的 Qt 事件循环线程调用，不支持并发提交。
class OcrClient : public QObject {
    Q_OBJECT
public:
    explicit OcrClient(QString module, int timeoutMs = 60000, int idleMs = 60000);
    // 等待原生推理和模块释放完成；必须在客户端所属线程而非工作线程析构。
    ~OcrClient() override;
    // 提交绝对可读图像路径，返回 job_id 或错误；图像须保留至后台不再使用。
    QJsonObject Submit(const QString& image);
    // 仅保留最近一次任务；返回其状态与终态回复，未知 ID 返回错误。
    QJsonObject Result(const QString& id) const;
    // 返回任务状态、后端可用性以及工作与回收标志。
    QJsonObject Status() const;
    // 使匹配的待处理任务失败；不强制终止推理，后台资源随后安全回收。
    QJsonObject Cancel(const QString& id);
    // 返回环境变量覆盖的模块路径，未设置时使用编译期默认路径。
    static QString DefaultModule();
    // 配置并异步校验模型目录；已加载或正在工作时忽略调用。
    void ConfigureModels(const QJsonObject& config, const QString& configError = { });

signals:
    // 任务终态的逻辑回复；取消或超时后原生推理仍可能运行。
    void Finished(const QString& request, const QJsonObject& response);
    // 后端已释放，可释放被取消或超时任务仍借用的图像文件。
    void WorkerStopped();

private:
    void SendRequest();
    void Fail(const QString& error, const QString& code = QStringLiteral("backend_error"));
    void StartWorker();
    void ReleaseWorker();
    bool Pending() const { return _state == QStringLiteral("pending"); }
    class Worker;
    Worker* _worker;
    QThread _thread;
    QString _module, _id, _image, _state = QStringLiteral("idle");
    QJsonObject _reply;
    QTimer _deadline, _idle;
    bool _loaded = false, _working = false, _retiring = false;
    bool _configured = false, _validated = false;
    QString _availability = QStringLiteral("unchecked"), _error, _errorCode;
    QString _detectionDir = QStringLiteral("/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det");
    QString _recognitionDir = QStringLiteral("/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec");
};
