#pragma once
#include <QStringList>

inline QStringList ExpectedControlTabs()
{
    QStringList tabs{QStringLiteral("系统"), QStringLiteral("交互"), QStringLiteral("模型")};
#if DESKWAIFU_TEST_SPEECH_PLUGIN
    tabs.append(QStringLiteral("语音"));
#endif
    tabs.append(QStringLiteral("日志"));
    return tabs;
}
