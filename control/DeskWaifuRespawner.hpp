#pragma once

#include "plugin/GuiHostServices.hpp"
#include <QElapsedTimer>
#include <QTimer>

class DeskWaifuRespawner : public QObject
{
    Q_OBJECT
public:
    explicit DeskWaifuRespawner(GuiHostServices& client, const QString& applicationDirectory,
                               QObject* parent = nullptr);
    ~DeskWaifuRespawner() override;
    bool isRunning() const { return _timer.isActive(); }
    // 允许注入进程目录、锁目录和时限，以保持测试隔离。
    void start(const QString& procDirectory = QStringLiteral("/proc"),
               const QString& lockDirectory = {}, int timeoutMs = 10000);

signals:
    void runningChanged(bool running);
    void finished(bool success, const QString& message);

private:
    void finish(bool success, const QString& message);
    GuiHostServices& _client;
    QString _applicationDirectory;
    QTimer _timer;
    QElapsedTimer _elapsed;
    bool _disconnected = false;
};
