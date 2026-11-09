// 提供避免泄漏响应敏感内容的 GUI 日志格式化函数。
#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// 返回仅含命令结果及显式白名单无害字段的紧凑 JSON 日志。
// 未知命令仅记录命令名与结果；不得将原始 IPC 响应直接传给 Qt 日志。
inline QString FormatGuiResponseLog(const QString& command, const QJsonObject& response)
{
    QJsonObject safe{{QStringLiteral("command"), command}};
    const auto ok = response.value(QStringLiteral("ok"));
    safe.insert(QStringLiteral("status"), ok.isBool()
        ? (ok.toBool() ? QStringLiteral("success") : QStringLiteral("failure"))
        : QStringLiteral("unknown"));

    QStringList allowed;
    if (command == QStringLiteral("get_render_device") || command == QStringLiteral("set_render_device"))
        allowed = {QStringLiteral("device"), QStringLiteral("active_device"), QStringLiteral("restart_required")};
    else if (command == QStringLiteral("get_mouse_look") || command == QStringLiteral("set_mouse_look"))
        allowed = {QStringLiteral("radius")};
    else if (command == QStringLiteral("get_hover_opacity") || command == QStringLiteral("set_hover_opacity"))
        allowed = {QStringLiteral("enabled"), QStringLiteral("opacity"), QStringLiteral("idle_opacity")};
    else if (command == QStringLiteral("get_transform_save_result"))
        allowed = {QStringLiteral("known")};

    for (const auto& key : allowed)
    {
        const auto value = response.value(key);
        if (value.isBool() || value.isDouble() || value.isString()) safe.insert(key, value);
    }
    return QString::fromUtf8(QJsonDocument(safe).toJson(QJsonDocument::Compact));
}
