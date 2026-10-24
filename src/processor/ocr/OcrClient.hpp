#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

// Only QtCore is needed by the main application; inference lives in the optional helper.
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

signals:
    void Finished(const QString& request, const QJsonObject& response);
    void WorkerStopped();

private:
    void ReadOutput();
    void SendRequest();
    void Fail(const QString& error);
    bool Pending() const { return _state == QStringLiteral("pending"); }
    QString _executable, _id, _image, _state = QStringLiteral("idle");
    QJsonObject _reply;
    QProcess _process;
    QTimer _deadline, _idle;
    QByteArray _buffer;
    bool _ready = false;
};
