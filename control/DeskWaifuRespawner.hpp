#pragma once

#include "DeskWaifuClient.hpp"
#include <QElapsedTimer>

class DeskWaifuRespawner : public QObject
{
    Q_OBJECT
public:
    explicit DeskWaifuRespawner(DeskWaifuClient& client, const QString& applicationDirectory,
                               QObject* parent = nullptr);
    ~DeskWaifuRespawner() override;
    bool isRunning() const { return _timer.isActive(); }
    // Injectable process/lock directories and deadline keep tests isolated.
    void start(const QString& procDirectory = QStringLiteral("/proc"),
               const QString& lockDirectory = {}, int timeoutMs = 10000);

signals:
    void runningChanged(bool running);
    void finished(bool success, const QString& message);

private:
    void finish(bool success, const QString& message);
    DeskWaifuClient& _client;
    QString _applicationDirectory;
    QTimer _timer;
    QElapsedTimer _elapsed;
    bool _disconnected = false;
};
