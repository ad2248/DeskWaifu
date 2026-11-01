#pragma once

#include <QJsonObject>
#include <QObject>
#include <QThread>
#include <QTimer>

// 主程序仅需 QtCore；可选模块的加载、推理和释放均在专用线程完成。
class OcrClient : public QObject
{
    Q_OBJECT
public:
    explicit OcrClient(QString module, int timeoutMs = 60000, int idleMs = 60000);
    ~OcrClient() override;
    QJsonObject Submit(const QString& image);
    QJsonObject Result(const QString& id) const;
    QJsonObject Status() const;
    QJsonObject Cancel(const QString& id);
    static QString DefaultModule();
    void ConfigureModels(const QJsonObject& config, const QString& configError = {});

signals:
    void Finished(const QString& request, const QJsonObject& response);
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
