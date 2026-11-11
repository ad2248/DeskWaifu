#include "VoiceInputTab.hpp"
#include "plugin/GuiHostServices.hpp"

#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QtTest>

class Services final : public GuiHostServices {
public:
    bool connected = true;
    bool active = true;
    QList<QPair<QString, QJsonObject>> commands;
    QStringList cancelled;
    bool isConnected() const override { return connected; }
    bool isActive() const override { return active; }
    void setActive(bool value)
    {
        active = value;
        emit activationChanged(value);
    }
    void sendCommand(const QString& command, const QJsonObject& parameters = { }) override { commands.append({ command, parameters }); }
    void cancelQueuedCommand(const QString& command) override { cancelled.append(command); }
    void reply(const QString& command, const QJsonObject& response) { emit responseReceived(command, response); }
    void setConnected(bool value)
    {
        connected = value;
        emit connectionChanged(value, { });
    }
    int count(const QString& command) const
    {
        int result = 0;
        for (const auto& entry : commands) {
            if (entry.first == command) {
                ++result;
            }
        }
        return result;
    }
    QJsonObject parameters(const QString& command) const
    {
        for (auto it = commands.crbegin(); it != commands.crend(); ++it) {
            if (it->first == command) {
                return it->second;
            }
        }
        return { };
    }
};

template <class T> T* widget(QWidget& page, const char* name)
{
    auto* result = page.findChild<T*>(QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}

static QJsonObject settings(bool configured = true)
{
    return { { "ok", true }, { "configured", configured }, { "endpoint", "https://asr.example/transcribe" }, { "model", "asr-model" }, { "device_id", "mic-1" },
        { "max_recording_seconds", 90 } };
}

static QJsonObject status(const QString& state = QStringLiteral("idle"), const QString& session = { })
{
    return { { "ok", true }, { "state", state }, { "phase", state }, { "session_id", session }, { "elapsed_ms", 1250 }, { "last_outcome", "" }, { "error_code", "" },
        { "error", "" }, { "copy_state", "idle" } };
}

class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void deferredStartAndPublicSettings()
    {
        Services services;
        VoiceInputTab page(services);
        QVERIFY(services.commands.isEmpty());
        page.start();
        page.start();
        QCOMPARE(services.count("get_voice_input_settings"), 1);
        QCOMPARE(services.count("get_voice_input_status"), 1);
        QCOMPARE(services.count("list_voice_input_devices"), 1);
        auto response = settings();
        response.insert("api_key", "must-never-be-displayed");
        services.reply("get_voice_input_settings", response);
        QCOMPARE(widget<QLineEdit>(page, "voiceInputEndpoint")->text(), QStringLiteral("https://asr.example/transcribe"));
        QCOMPARE(widget<QLineEdit>(page, "voiceInputModel")->text(), QStringLiteral("asr-model"));
        auto* maxSeconds = widget<QSpinBox>(page, "voiceInputMaxRecordingSeconds");
        QCOMPARE(maxSeconds->value(), 90);
        QCOMPARE(maxSeconds->minimum(), 1);
        QCOMPARE(maxSeconds->maximum(), 300);
        QVERIFY(!widget<QLineEdit>(page, "voiceInputEndpoint")->placeholderText().contains("audio/transcriptions"));
        QCOMPARE(widget<QLineEdit>(page, "voiceInputApiKey")->echoMode(), QLineEdit::Password);
        QVERIFY(widget<QLineEdit>(page, "voiceInputApiKey")->text().isEmpty());
        QVERIFY(!widget<QLabel>(page, "voiceInputSettingsStatus")->text().contains("must-never"));
    }

    void savesEndpointAndKeyWithoutEcho()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        widget<QLineEdit>(page, "voiceInputEndpoint")->setText(" https://custom.example/asr ");
        auto* key = widget<QLineEdit>(page, "voiceInputApiKey");
        key->setText(" secret-key ");
        auto* save = widget<QPushButton>(page, "saveVoiceInputSettings");
        save->click();
        QVERIFY(key->text().isEmpty());
        QVERIFY(!save->isEnabled());
        QCOMPARE(services.parameters("set_voice_input_settings").value("endpoint").toString(), QStringLiteral("https://custom.example/asr"));
        QCOMPARE(services.parameters("set_voice_input_settings").value("api_key").toString(), QStringLiteral("secret-key"));
        QCOMPARE(services.parameters("set_voice_input_settings").value("device_id").toString(), QStringLiteral("mic-1"));
        QCOMPARE(services.parameters("set_voice_input_settings").value("max_recording_seconds").toInt(), 90);
        // An old query cannot unlock the save operation or overwrite the editor.
        services.reply("get_voice_input_settings", settings(false));
        QVERIFY(!save->isEnabled());
        QCOMPARE(widget<QLineEdit>(page, "voiceInputEndpoint")->text(), QStringLiteral(" https://custom.example/asr "));
        auto response = settings();
        response.insert("api_key", "secret-key");
        services.reply("set_voice_input_settings", response);
        QVERIFY(save->isEnabled());
        QVERIFY(key->text().isEmpty());
        QVERIFY(!widget<QLabel>(page, "voiceInputSettingsStatus")->text().contains("secret-key"));
    }

    void emptyKeyPreservesAndExplicitClearSendsEmptyKey()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        widget<QPushButton>(page, "saveVoiceInputSettings")->click();
        QVERIFY(!services.parameters("set_voice_input_settings").contains("api_key"));
        services.reply("set_voice_input_settings", settings());
        widget<QLineEdit>(page, "voiceInputApiKey")->setText("do-not-save-this");
        widget<QPushButton>(page, "clearVoiceInputKey")->click();
        QVERIFY(services.parameters("set_voice_input_settings").contains("api_key"));
        QCOMPARE(services.parameters("set_voice_input_settings").value("api_key").toString(), QString());
        QVERIFY(widget<QLineEdit>(page, "voiceInputApiKey")->text().isEmpty());
        services.reply("set_voice_input_settings", settings(false));
        QVERIFY(!widget<QPushButton>(page, "clearVoiceInputKey")->isEnabled());
    }

    void disconnectDoesNotReplayMutations()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        services.reply("get_voice_input_status", status());
        widget<QLineEdit>(page, "voiceInputApiKey")->setText("key");
        widget<QPushButton>(page, "saveVoiceInputSettings")->click();
        services.setConnected(false);
        QVERIFY(services.cancelled.contains("set_voice_input_settings"));
        QVERIFY(!widget<QPushButton>(page, "saveVoiceInputSettings")->isEnabled());
        QVERIFY(widget<QLineEdit>(page, "voiceInputApiKey")->text().isEmpty());
        services.setConnected(true);
        QCOMPARE(services.count("set_voice_input_settings"), 1);
        QCOMPARE(services.count("get_voice_input_settings"), 2);
        services.reply("get_voice_input_settings", settings());
        services.reply("get_voice_input_status", status());
        widget<QPushButton>(page, "startVoiceInput")->click();
        QCOMPARE(services.count("start_voice_input"), 1);
        services.setConnected(false);
        services.setConnected(true);
        QCOMPARE(services.count("start_voice_input"), 1);
        QVERIFY(services.cancelled.contains("start_voice_input"));
    }

    void sessionIdsAndExplicitClipboardRetry()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        const QString id = QStringLiteral("18446744073709551615");
        services.reply("get_voice_input_status", status("recording", id));
        widget<QPushButton>(page, "stopVoiceInput")->click();
        QCOMPARE(services.parameters("stop_voice_input").value("session_id").toString(), id);
        QVERIFY(services.parameters("stop_voice_input").value("session_id").isString());
        services.reply("stop_voice_input", { { "ok", true } });
        services.reply("get_voice_input_status", status("recording", id));
        widget<QPushButton>(page, "cancelVoiceInput")->click();
        QCOMPARE(services.parameters("cancel_voice_input").value("session_id").toString(), id);
        services.reply("cancel_voice_input", { { "ok", true } });
        services.reply("get_voice_input_status", status("idle", id));
        QCOMPARE(services.parameters("get_voice_input_result").value("session_id").toString(), id);
        services.reply("get_voice_input_result", { { "ok", true }, { "session_id", id }, { "text", "识别文本" }, { "copy_state", "failed" } });
        QCOMPARE(widget<QPlainTextEdit>(page, "voiceInputResult")->toPlainText(), QStringLiteral("识别文本"));
        QCOMPARE(services.count("retry_voice_input_copy"), 0);
        widget<QPushButton>(page, "retryVoiceInputCopy")->click();
        QCOMPARE(services.parameters("retry_voice_input_copy").value("session_id").toString(), id);
        QVERIFY(!services.parameters("retry_voice_input_copy").contains("text"));
        services.reply("retry_voice_input_copy", { { "ok", false } });
        QVERIFY(widget<QPushButton>(page, "retryVoiceInputCopy")->isEnabled());
    }

    void resultReadRetriesAfterBusyForSameSession()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        services.reply("get_voice_input_status", status("idle", "42"));
        QCOMPARE(services.count("get_voice_input_result"), 1);
        services.reply("get_voice_input_result", { { "ok", false }, { "error_code", "busy" } });
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 2, 1200);
        services.reply("get_voice_input_status", status("idle", "42"));
        QCOMPARE(services.count("get_voice_input_result"), 2);
        QCOMPARE(services.parameters("get_voice_input_result").value("session_id").toString(), QStringLiteral("42"));
        services.reply("get_voice_input_result", { { "ok", true }, { "session_id", "42" }, { "text", "重试读取成功" }, { "copy_state", "committed" } });
        QCOMPARE(widget<QPlainTextEdit>(page, "voiceInputResult")->toPlainText(), QStringLiteral("重试读取成功"));
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 3, 1200);
        services.reply("get_voice_input_status", status("idle", "42"));
        QCOMPARE(services.count("get_voice_input_result"), 2);
    }

    void reconnectRequiresFreshSessionAndMutationRequiresFreshStatus()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        services.reply("get_voice_input_status", status("recording", "old-session"));
        QVERIFY(widget<QPushButton>(page, "stopVoiceInput")->isEnabled());
        services.setConnected(false);
        services.setConnected(true);
        QVERIFY(!widget<QPushButton>(page, "stopVoiceInput")->isEnabled());
        QVERIFY(!widget<QPushButton>(page, "cancelVoiceInput")->isEnabled());
        services.reply("get_voice_input_status", status());
        widget<QPushButton>(page, "startVoiceInput")->click();
        services.reply("start_voice_input", { { "ok", true } });
        QVERIFY(!widget<QPushButton>(page, "startVoiceInput")->isEnabled());
        services.reply("get_voice_input_status", status("recording", "new-session"));
        widget<QPushButton>(page, "stopVoiceInput")->click();
        QCOMPARE(services.parameters("stop_voice_input").value("session_id").toString(), QStringLiteral("new-session"));
    }

    void preMutationStatusCannotConfirmRecordingOutcome()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        services.reply("get_voice_input_status", status());
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 2, 1200);
        widget<QPushButton>(page, "startVoiceInput")->click();
        services.reply("start_voice_input", { { "ok", true } });
        services.reply("get_voice_input_status", status());
        QVERIFY(!widget<QPushButton>(page, "startVoiceInput")->isEnabled());
        QCOMPARE(services.count("get_voice_input_status"), 3);
        services.reply("get_voice_input_status", status("recording", "new-session"));
        QVERIFY(widget<QPushButton>(page, "stopVoiceInput")->isEnabled());
    }

    void pollingNeverAccumulatesAndStopOnlyCancelsUnsent()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        QTest::qWait(1100);
        QCOMPARE(services.count("get_voice_input_status"), 1);
        services.reply("get_voice_input_status", status("recording", "session"));
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 2, 1200);
        QTest::qWait(650);
        QCOMPARE(services.count("get_voice_input_status"), 2);
        page.stop();
        const auto count = services.commands.size();
        QVERIFY(services.cancelled.contains("get_voice_input_status"));
        QCOMPARE(services.count("cancel_voice_input"), 0);
        services.reply("get_voice_input_status", status());
        services.setConnected(false);
        services.setConnected(true);
        QTest::qWait(650);
        QCOMPARE(services.commands.size(), count);
    }

    void zeroSessionAndStaleResultDoNotBlockNextSession()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_status", status("idle", "0"));
        QCOMPARE(services.count("get_voice_input_result"), 0);
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 2, 1200);
        services.reply("get_voice_input_status", status("idle", "1"));
        QCOMPARE(services.count("get_voice_input_result"), 1);
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 3, 1200);
        services.reply("get_voice_input_status", status("idle", "2"));
        // The single older request blocks a second result read until it completes.
        QCOMPARE(services.count("get_voice_input_result"), 1);
        services.reply("get_voice_input_result", { { "ok", false }, { "session_id", "2" }, { "error_code", "stale_session" } });
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 4, 1200);
        services.reply("get_voice_input_status", status("idle", "2"));
        QCOMPARE(services.count("get_voice_input_result"), 2);
        QCOMPARE(services.parameters("get_voice_input_result").value("session_id").toString(), QStringLiteral("2"));
        services.reply("get_voice_input_result", { { "ok", true }, { "session_id", "2" }, { "text", "new text" } });
        QCOMPARE(widget<QPlainTextEdit>(page, "voiceInputResult")->toPlainText(), QStringLiteral("new text"));
    }

    void previousSessionSuccessCannotOverwriteCurrentResult()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_status", status("idle", "1"));
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 2, 1200);
        services.reply("get_voice_input_status", status("recording", "2"));
        services.reply("get_voice_input_result", { { "ok", true }, { "session_id", "1" }, { "text", "old text" } });
        QVERIFY(widget<QPlainTextEdit>(page, "voiceInputResult")->toPlainText().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 3, 1200);
        services.reply("get_voice_input_status", status("idle", "2"));
        QCOMPARE(services.count("get_voice_input_result"), 2);
        services.reply("get_voice_input_result", { { "ok", true }, { "session_id", "2" }, { "text", "current text" } });
        QCOMPARE(widget<QPlainTextEdit>(page, "voiceInputResult")->toPlainText(), QStringLiteral("current text"));
    }

    void hiddenPageAndInactiveApplicationPausePollingOnly()
    {
        Services services;
        VoiceInputTab page(services);
        page.show();
        page.start();
        services.reply("get_voice_input_status", status("recording", "1"));
        page.hide();
        QTest::qWait(650);
        QCOMPARE(services.count("get_voice_input_status"), 1);
        QCOMPARE(services.count("cancel_voice_input"), 0);
        page.show();
        QCOMPARE(services.count("get_voice_input_status"), 2);
        services.setActive(false);
        // An already in-flight response must not restart polling while inactive.
        services.reply("get_voice_input_status", status("recording", "1"));
        QTest::qWait(650);
        QCOMPARE(services.count("get_voice_input_status"), 2);
        QCOMPARE(services.count("cancel_voice_input"), 0);
        services.setActive(true);
        QCOMPARE(services.count("get_voice_input_status"), 3);
        QCOMPARE(services.count("start_voice_input"), 0);
        QCOMPARE(services.count("stop_voice_input"), 0);
        QCOMPARE(services.count("cancel_voice_input"), 0);
    }

    void largeResultRetrySendsOnlyDisplayedResultSession()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_status", status("idle", "1"));
        const QString text = QStringLiteral("line\n").repeated(32768);
        QVERIFY(text.toUtf8().size() > 64 * 1024);
        services.reply("get_voice_input_result", { { "ok", true }, { "session_id", "1" }, { "text", text } });
        QCOMPARE(widget<QPlainTextEdit>(page, "voiceInputResult")->toPlainText(), text);
        QTRY_COMPARE_WITH_TIMEOUT(services.count("get_voice_input_status"), 2, 1200);
        services.reply("get_voice_input_status", status("recording", "2"));
        widget<QPushButton>(page, "retryVoiceInputCopy")->click();
        const auto parameters = services.parameters("retry_voice_input_copy");
        // The displayed text belongs to session 1, not the current recording 2.
        QCOMPARE(parameters, QJsonObject({ { "session_id", "1" } }));
        QVERIFY(QJsonDocument(parameters).toJson(QJsonDocument::Compact).size() < 100);
        QCOMPARE(services.count("write_clipboard"), 0);
        services.reply("retry_voice_input_copy", { { "ok", false }, { "session_id", "2" }, { "error_code", "stale_session" } });
        QVERIFY(widget<QLabel>(page, "voiceInputCopyStatus")->text().contains("stale_session"));
        QCOMPARE(services.count("retry_voice_input_copy"), 1);
        services.setConnected(false);
        services.setConnected(true);
        QCOMPARE(services.count("retry_voice_input_copy"), 1);
    }

    void deviceListPreservesConfiguredSelection()
    {
        Services services;
        VoiceInputTab page(services);
        page.start();
        services.reply("get_voice_input_settings", settings());
        services.reply("list_voice_input_devices",
            { { "ok", true },
                { "devices",
                    QJsonArray { QJsonObject { { "id", "mic-2" }, { "name", "Other" }, { "is_default", true } },
                        QJsonObject { { "id", "mic-1" }, { "name", "Selected" }, { "is_default", false } } } } });
        auto* device = widget<QComboBox>(page, "voiceInputDevice");
        QCOMPARE(device->currentData().toString(), QStringLiteral("mic-1"));
        QCOMPARE(device->currentText(), QStringLiteral("Selected"));
    }
};

QTEST_MAIN(GuiTest)
#include "GuiTest.moc"
