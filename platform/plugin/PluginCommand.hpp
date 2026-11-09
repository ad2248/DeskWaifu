#pragma once

#include <QString>

// Execute a plugin-owned command, or return -1 to continue normal startup.
// 按环境变量、可执行文件相邻目录、defaultPluginRoot 的优先级查找命令。
// 找到唯一提供者后以 execv 替换当前进程；没有匹配返回 -1，歧义或执行失败返回 1。
int RunPluginCommand(int argc, char* argv[], const QString& defaultPluginRoot);
