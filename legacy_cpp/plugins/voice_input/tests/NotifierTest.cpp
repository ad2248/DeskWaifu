#include "private/VoiceInputNotifier.hpp"

#include <QDBusVirtualObject>
#include <QtTest>

class Notifications final : public QDBusVirtualObject {
public:
    QList<QDBusMessage> calls;
    uint nextId = 40;
    bool delayReply = false;
    QString introspect(const QString&) const override { return { }; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.member() != "Notify") {
            return false;
        }
        message.setDelayedReply(true);
        calls.append(message);
        if (!delayReply) {
            connection.send(message.createReply({ QVariant::fromValue(nextId++) }));
        }
        return true;
    }
};

class NotifierTest : public QObject {
    Q_OBJECT
private slots:
    void freshBoundarySurvivesCoalescedCancellationAndRestart()
    {
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "notification-test-server");
        Notifications service;
        service.delayReply = true;
        QVERIFY(bus.registerService("org.freedesktop.Notifications"));
        QVERIFY(bus.registerVirtualObject("/org/freedesktop/Notifications", &service));
        {
            VoiceInputNotifier notifier;
            notifier.ShowState({ { "phase", "recording" }, { "session_id", "1" } });
            QTRY_COMPARE(service.calls.size(), 1);
            notifier.ShowState({ { "phase", "idle" }, { "last_outcome", "cancelled" }, { "session_id", "1" } });
            notifier.ShowState({ { "phase", "recording" }, { "session_id", "2" } });
            service.delayReply = false;
            QVERIFY(bus.send(service.calls[0].createReply({ QVariant::fromValue(service.nextId++) })));
            QTRY_COMPARE(service.calls.size(), 2);
            QCOMPARE(service.calls[1].arguments()[1].toUInt(), uint(0));
        }
        bus.unregisterObject("/org/freedesktop/Notifications");
        bus.unregisterService("org.freedesktop.Notifications");
    }

    void freshNotificationsForEachRecordingAndCompletion()
    {
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "notification-test-server");
        Notifications service;
        QVERIFY(bus.registerService("org.freedesktop.Notifications"));
        QVERIFY(bus.registerVirtualObject("/org/freedesktop/Notifications", &service));
        {
            VoiceInputNotifier notifier;
            const auto send = [&](const QString& phase, const QString& outcome = QString()) {
                const int expected = service.calls.size() + 1;
                notifier.ShowState({ { "phase", phase }, { "last_outcome", outcome }, { "text", "private transcript must never be sent" } });
                QTRY_COMPARE(service.calls.size(), expected);
                // Let the asynchronous reply release the notifier's pending call.
                QCoreApplication::processEvents();
            };
            send("recording");
            send("recording");
            send("transcribing");
            send("copying");
            send("idle", "copied");
            send("recording");
            send("idle", "cancelled");
            send("copying");
            send("idle", "copied"); // Explicit copy retry must also pop up.
            const QList<uint> expected { 0, 40, 41, 42, 0, 0, 0, 46, 0 };
            QCOMPARE(service.calls.size(), expected.size());
            for (int i = 0; i < expected.size(); ++i) {
                const auto& call = service.calls[i];
                QCOMPARE(call.signature(), QStringLiteral("susssasa{sv}i"));
                QCOMPARE(call.arguments()[1].toUInt(), expected[i]);
                QVERIFY(!call.arguments()[3].toString().contains("private transcript"));
                QVERIFY(call.arguments()[4].toString().isEmpty());
            }
            QCOMPARE(service.calls[4].arguments()[3].toString(), QStringLiteral("语音识别完成，已复制到剪贴板"));
        }
        bus.unregisterObject("/org/freedesktop/Notifications");
        bus.unregisterService("org.freedesktop.Notifications");
    }
};
QTEST_GUILESS_MAIN(NotifierTest)
#include "NotifierTest.moc"
