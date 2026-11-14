#pragma once

#include <QString>

// Installs the process-wide Qt message handler and truncates the process log
// for this launch. The directory is shared with child processes through
// DESKWAIFU_LOG_DIR when the GUI starts the daemon.
// 安装进程级 Qt 日志处理器并截断本次启动的日志文件，返回目标日志路径。
// 通过 DESKWAIFU_LOG_DIR 与子进程共享目录；即使文件打开失败仍输出到 stderr，
// 因此返回路径不代表文件已成功创建。应在进程启动时初始化。
QString InitializeQtLogging(const QString& fileName);
