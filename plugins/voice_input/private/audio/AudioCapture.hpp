#pragma once
#include "PcmNormalizer.hpp"
#include <QAudioSource>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <memory>

class AudioCapture : public QObject {
    Q_OBJECT
public:
    explicit AudioCapture(QObject* parent = nullptr);
    ~AudioCapture() override;
    virtual bool Start(quint64 sessionId, const QString& deviceId, int maxSeconds, QString& error);
    virtual bool Stop(quint64 sessionId, QByteArray& pcm, QString& error);
    virtual void Cancel();
    static QJsonArray ListDevices();
signals:
    void Failed(quint64 sessionId, const QString& code, const QString& message);

private:
    friend class AudioTest; // Offline pull-device tests, without enumerating or opening hardware.
    static QJsonObject DeviceInfo(const QByteArray& id, const QString& name, bool isDefault);
    bool Drain(QString& error);
    void OnAudioReady();
    void QueueFailure(const QString& code, const QString& message);
    void Fail(const QString& code, const QString& message);
    std::unique_ptr<QAudioSource> _source;
    QIODevice* _device = nullptr;
    PcmNormalizer _normalizer;
    QTimer _limit;
    QElapsedTimer _elapsed;
    quint64 _sessionId = 0, _generation = 0;
    QString _pendingFailure;
    qint64 _inputBytes = 0, _maxInputBytes = 0;
    int _maxSeconds = 0;
    bool _active = false, _budgetReached = false;
};
