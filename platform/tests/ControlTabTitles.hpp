#pragma once
#include <QStringList>

// 返回控制面板测试预期的标签顺序，可选插件标签随各自构建选项包含。
inline QStringList ExpectedControlTabs()
{
    QStringList tabs{QStringLiteral("系统"), QStringLiteral("交互"), QStringLiteral("模型")};
#if DESKWAIFU_TEST_SPEECH_PLUGIN
    tabs.append(QStringLiteral("语音"));
#endif
#if DESKWAIFU_TEST_VOICE_INPUT_PLUGIN
    tabs.append(QStringLiteral("语音输入"));
#endif
    return tabs;
}
