#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonObject>
#include <QDebug>
#include <QObject>

// 通知仅显示脱敏状态；D-Bus/通知服务不可用不影响录音和识别。
class VoiceInputNotifier final : public QObject
{
public:
    explicit VoiceInputNotifier(QObject* parent = nullptr) : QObject(parent) {}
    void ShowState(const QJsonObject& status)
    {
        const auto phase = status.value("phase").toString();
        // Preserve lifecycle boundaries even when pending calls coalesce states.
        _freshPending = _freshPending || phase == "idle"
            || (phase == "recording" && _lastPhase != "recording");
        _lastPhase = phase;
        if (_pending) { _latest = status; return; }
        QString summary;
        if (phase == "recording") summary = tr("录音中：%1 秒").arg(status.value("elapsed_ms").toDouble() / 1000, 0, 'f', 0);
        else if (phase == "transcribing") summary = tr("正在识别语音…");
        else if (phase == "copying") summary = tr("正在写入剪贴板…");
        else if (phase == "idle") {
            const auto outcome = status.value("last_outcome").toString();
            if (outcome == "copied") summary = tr("语音识别完成，已复制到剪贴板");
            else if (outcome == "recognized_not_copied") summary = tr("语音已识别，复制失败，请在面板中重试");
            else if (outcome == "cancelled") summary = tr("语音输入已取消");
            else if (outcome == "failed") summary = tr("语音输入失败，请在控制面板查看状态");
        }
        if (summary.isEmpty()) return;
        // Plasma may silently update an expired notification when given its old ID.
        // Start each recording and each terminal outcome with a fresh popup;
        // only progress updates replace the current notification.
        const bool fresh = _freshPending;
        _freshPending = false;
        auto message = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "Notify");
        message.setArguments({QStringLiteral("DeskWaifu"), fresh ? uint(0) : _notificationId, QString{}, summary, QString{},
            QStringList{}, QVariantMap{}, 5000});
        _pending = true;
        auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            const QDBusPendingReply<uint> reply = *watcher;
            if (!reply.isError()) _notificationId = reply.value();
            else qWarning() << "Voice input desktop notification failed:" << reply.error().name();
            _pending = false; watcher->deleteLater();
            if (!_latest.isEmpty()) {
                const auto latest = _latest; _latest = {};
                ShowState(latest);
            }
        });
    }
private:
    uint _notificationId = 0;
    QString _lastPhase;
    bool _pending = false;
    bool _freshPending = false;
    QJsonObject _latest;
};
