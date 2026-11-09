#pragma once
#include <QString>
// 读取启动配置、命令行和环境变量，并设置模型目录及渲染设备。
// 应在桌面处理器初始化前调用；失败返回 false，并在 error 非空时写入诊断。
// 配置文件无法打开且没有解析诊断时允许使用默认值。
bool ConfigureDesktop(QString* error);
