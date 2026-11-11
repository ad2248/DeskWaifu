#pragma once

#include "plugin/GuiHostServices.hpp"
#include <QElapsedTimer>
#include <QTimer>

// 在 GUI 线程协调退出、等待旧后端消失和重新启动，不把断线等同于退出。
// 借用 client，调用方须保证其比此对象存活更久。
class DeskWaifuRespawner : public QObject {
    Q_OBJECT
public:
    explicit DeskWaifuRespawner(GuiHostServices& client, const QString& applicationDirectory, QObject* parent = nullptr);
    ~DeskWaifuRespawner() override;
    // 返回是否正在等待退出或尝试重新启动，不代表后端进程运行状态。
    bool isRunning() const { return _timer.isActive(); }
    // 需已连接且能找到可执行文件；重复启动被忽略，结果通过 finished 报告。
    // 允许注入进程目录、锁目录和时限，以保持测试隔离。
    void start(const QString& procDirectory = QStringLiteral("/proc"), const QString& lockDirectory = { }, int timeoutMs = 10000);
    // Silent, idempotent teardown; never starts a replacement or emits feedback.
    // 取消尚未发送的退出命令，静默停止重启流程；已经发送的退出无法撤回。
    void stop() noexcept;

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
