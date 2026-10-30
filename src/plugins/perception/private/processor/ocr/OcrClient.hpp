#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

// 主程序仅需 QtCore；推理由可选辅助进程完成。
class OcrClient : public QObject
{
    Q_OBJECT
public:
    explicit OcrClient(QString executable, int timeoutMs = 60000, int idleMs = 60000);
    ~OcrClient() override;
    QJsonObject Submit(const QString& image);
    QJsonObject Result(const QString& id) const;
    QJsonObject Status() const;
    QJsonObject Cancel(const QString& id);
    static QString DefaultExecutable();
    void ConfigureModels(const QJsonObject& config, const QString& configError = {});

signals:
    void Finished(const QString& request, const QJsonObject& response);
    void WorkerStopped();

private:
    void ReadOutput();
    void SendRequest();
    void Fail(const QString& error, const QString& code = QStringLiteral("helper_error"));
    void StartWorker();
    bool Pending() const { return _state == QStringLiteral("pending"); }
    QString _executable, _id, _image, _state = QStringLiteral("idle");
    QJsonObject _reply;
    QProcess _process;
    QTimer _deadline, _idle;
    QByteArray _buffer;
    bool _ready = false;
    bool _configured = false, _checking = false, _validated = false;
    QString _availability = QStringLiteral("unchecked"), _error, _errorCode;
    QString _detectionDir, _recognitionDir;
};
