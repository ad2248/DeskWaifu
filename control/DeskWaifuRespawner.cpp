#include "DeskWaifuRespawner.hpp"
#include "DeskWaifuLauncher.hpp"

DeskWaifuRespawner::DeskWaifuRespawner(GuiHostServices& client, const QString& applicationDirectory,
                                     QObject* parent)
    : QObject(parent), _client(client), _applicationDirectory(applicationDirectory)
{
    _timer.setInterval(100);
    connect(&_client, &GuiHostServices::connectionChanged, this, [this](bool connected, const QString&) {
        if (isRunning() && !connected) _disconnected = true;
    });
    connect(&_client, &GuiHostServices::responseReceived, this,
        [this](const QString& command, const QJsonObject& response) {
            if (isRunning() && command == QStringLiteral("quit") && !response.value("ok").toBool())
                finish(false, tr("重启失败，退出请求被拒绝：%1").arg(response.value("error").toString()));
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
        emit finished(false, tr("重启需要先连接 DeskWaifu，请使用“启动”按钮启动。"));
        return;
    }
    if (DeskWaifuLauncher::executablePath(_applicationDirectory).isEmpty()) {
        emit finished(false, tr("在控制面板所在目录或 PATH 中找不到 deskwaifu，未停止正在运行的后端。"));
        return;
    }
    _disconnected = false;
    _elapsed.start();
    disconnect(&_timer, nullptr, this, nullptr);
    connect(&_timer, &QTimer::timeout, this, [this, procDirectory, lockDirectory, timeoutMs] {
        if (_elapsed.elapsed() >= timeoutMs) {
            finish(false, tr("重启超时：等待 DeskWaifu 退出超时，未启动新进程。"));
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
