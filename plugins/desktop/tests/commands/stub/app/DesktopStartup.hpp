#pragma once

class QString;
// 命令行测试跳过真实桌面平台配置，总是成功且不写入错误字符串。
inline bool ConfigureDesktop(QString*) { return true; }
