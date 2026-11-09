#pragma once

#include <QString>

// 查找同环境后端并以跨进程锁避免重复启动，不接管后端进程生命周期。
namespace DeskWaifuLauncher
{
enum class State { Connected, AlreadyRunning, Started, Failed };

// 启动检查结果；AlreadyRunning 或 Started 时携带 PID，message 用于界面反馈。
struct Result
{
    State state;
    QString message;
    qint64 pid = 0;
};

// 优先查找应用目录内可执行的 deskwaifu，再查 PATH；未找到时返回空字符串。
QString executablePath(const QString& applicationDirectory);
// 仅检查同 UID、同 XDG_RUNTIME_DIR 的后端；不存在时返回 0，无法检查时返回 -1。
qint64 runningPid(const QString& procDirectory = QStringLiteral("/proc"));
// 已连接时不启动；否则持锁检查进程，必要时启动独立于 GUI 的分离进程。
// 返回启动或检查结果，Started 不保证 IPC 已就绪。
// 允许注入目录，以隔离测试中的进程夹具和跨进程锁。
Result spawn(bool connected, const QString& applicationDirectory,
             const QString& procDirectory = QStringLiteral("/proc"),
             const QString& lockDirectory = {});
}
