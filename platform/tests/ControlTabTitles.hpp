#pragma once
#include <QStringList>

// 返回控制面板测试预期的标签顺序，语音标签仅在启用语音插件测试时包含。
inline QStringList ExpectedControlTabs()
{
    QStringList tabs{QStringLiteral("系统"), QStringLiteral("交互"), QStringLiteral("模型")};
#if DESKWAIFU_TEST_SPEECH_PLUGIN
    tabs.append(QStringLiteral("语音"));
#endif
    return tabs;
}
