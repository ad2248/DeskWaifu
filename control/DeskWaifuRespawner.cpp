#include "DeskWaifuRespawner.hpp"
#include "DeskWaifuLauncher.hpp"

DeskWaifuRespawner::DeskWaifuRespawner(DeskWaifuClient& client, const QString& applicationDirectory,
                                     QObject* parent)
    : QObject(parent), _client(client), _applicationDirectory(applicationDirectory)
{
    _timer.setInterval(100);
    connect(&_client, &DeskWaifuClient::connectionChanged, this, [this](bool connected, const QString&) {
        if (isRunning() && !connected) _disconnected = true;
    });
    connect(&_client, &DeskWaifuClient::responseReceived, this,
        [this](const QString& command, const QJsonObject& response) {
            if (isRunning() && command == QStringLiteral("quit") && !response.value("ok").toBool())
                finish(false, tr("Respawn failed: quit was rejected: %1").arg(response.value("error").toString()));
        });
}

DeskWaifuRespawner::~DeskWaifuRespawner()
{
    if (isRunning()) _client.cancelQueuedCommand(QStringLiteral("quit"));
}

void DeskWaifuRespawner::start(const QString& procDirectory, const QString& lockDirectory, int timeoutMs)
{
    if (isRunning()) return;
    if (!_client.isConnected()) {
        emit finished(false, tr("Respawn requires a connected DeskWaifu; use Spawn to start it."));
        return;
    }
    if (DeskWaifuLauncher::executablePath(_applicationDirectory).isEmpty()) {
        emit finished(false, tr("Cannot find deskwaifu next to the GUI or in PATH; the running backend was not stopped."));
        return;
    }
    _disconnected = false;
    _elapsed.start();
    disconnect(&_timer, nullptr, this, nullptr);
    connect(&_timer, &QTimer::timeout, this, [this, procDirectory, lockDirectory, timeoutMs] {
        if (_elapsed.elapsed() >= timeoutMs) {
            finish(false, tr("Respawn timed out waiting for DeskWaifu to exit; no replacement was started."));
            return;
        }
        if (!_disconnected) return;
        // IPC 套接字关闭不代表旧进程已清理完成。
        // 启动时的进程检查和共享锁可防止多个实例重叠运行。
        const auto result = DeskWaifuLauncher::spawn(false, _applicationDirectory, procDirectory, lockDirectory);
        if (result.state == DeskWaifuLauncher::State::AlreadyRunning) return;
        finish(result.state == DeskWaifuLauncher::State::Started, result.message);
        if (result.state == DeskWaifuLauncher::State::Started) _client.connectToServer();
    });
    _timer.start();
    emit runningChanged(true);
    _client.sendCommand(QStringLiteral("quit"));
}

void DeskWaifuRespawner::finish(bool success, const QString& message)
{
    _timer.stop();
    _client.cancelQueuedCommand(QStringLiteral("quit"));
    emit runningChanged(false);
    emit finished(success, message);
}
