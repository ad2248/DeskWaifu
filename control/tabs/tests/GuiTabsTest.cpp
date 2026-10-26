#include "SystemTab.hpp"
#include "InteractionTab.hpp"
#include "ModelTab.hpp"
#include "LogTab.hpp"
#include "SpeechTab.hpp"
#include "plugin/GuiHostServices.hpp"
#include "HotkeyPanel.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QtTest>

class Services final : public GuiHostServices
{
public:
    bool connected = true;
    QList<QPair<QString, QJsonObject>> commands;
    bool isConnected() const override { return connected; }
    void sendCommand(const QString& command, const QJsonObject& parameters = {}) override { commands.append({command, parameters}); }
    void cancelQueuedCommand(const QString&) override {}
    void reply(const QString& command, const QJsonObject& response) { emit responseReceived(command, response); }
    void disconnectBackend() { connected = false; emit connectionChanged(false, QStringLiteral("offline")); }
    int count(const QString& command) const {
        int result = 0;
        for (const auto& entry : commands) if (entry.first == command) ++result;
        return result;
    }
};

template<class T> T* widget(QWidget& page, const char* name)
{
    auto* result = page.findChild<T*>(QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}

class GuiTabsTest : public QObject
{
    Q_OBJECT
private slots:
    void statusReadinessAndProcessDirectory()
    {
        Services services;
        services.connected = false;
        QTemporaryDir emptyProc;
        services.setProperty("procDirectory", emptyProc.path());
        SystemTab system(services);
        QCOMPARE(widget<QPushButton>(system, "spawnButton")->text(), QStringLiteral("启动"));
        QVERIFY(!widget<QPushButton>(system, "respawnButton")->isEnabled());
        QSignalSpy ready(&services, &GuiHostServices::backendReady);
        services.reply(QStringLiteral("get_status"), {{"ok", false}});
        QCOMPARE(ready.count(), 0);
        services.reply(QStringLiteral("get_status"), {{"ok", true}, {"model", "A"}, {"model_count", 2}, {"hidden", true}});
        QCOMPARE(ready.count(), 1);
        QCOMPARE(widget<QLabel>(system, "currentModelLabel")->text(), QStringLiteral("A"));
        QCOMPARE(widget<QLabel>(system, "modelCountLabel")->text(), QStringLiteral("2"));
        QCOMPARE(widget<QLabel>(system, "visibilityLabel")->text(), QStringLiteral("已隐藏"));
    }

    void hotkeyTokenAndDuplicateLogging()
    {
        Services services;
        InteractionTab interaction(services);
        LogTab log(services);
        const QJsonObject bindings{{"ok", true}, {"model_token", "token-A"}, {"hotkeys", QJsonArray{}}};
        services.reply(QStringLiteral("get_hotkeys"), bindings);
        const auto logged = widget<QPlainTextEdit>(log, "responseLog")->toPlainText();
        services.reply(QStringLiteral("get_hotkeys"), bindings);
        QCOMPARE(widget<QPlainTextEdit>(log, "responseLog")->toPlainText(), logged);
        emit widget<HotkeyPanel>(interaction, "")->triggered(QStringLiteral("test-id"));
        QCOMPARE(services.commands.last().first, QStringLiteral("trigger_hotkey"));
        QCOMPARE(services.commands.last().second.value("model_token").toString(), QStringLiteral("token-A"));
        services.disconnectBackend();
        const auto count = services.commands.size();
        emit widget<HotkeyPanel>(interaction, "")->triggered(QStringLiteral("test-id"));
        QCOMPARE(services.commands.size(), count);
    }

    void deferredInitialQueries()
    {
        Services services;
        SystemTab system(services);
        InteractionTab interaction(services);
        ModelTab model(services);
        SpeechTab speech(services);
        QVERIFY(services.commands.isEmpty());
        system.start(); interaction.start(); model.start(); speech.start();
        QCOMPARE(services.count(QStringLiteral("get_status")), 1);
        QCOMPARE(services.count(QStringLiteral("get_hotkeys")), 1);
        QCOMPARE(services.count(QStringLiteral("get_transform_settings")), 1);
        QCOMPARE(services.count(QStringLiteral("get_speech_settings")), 1);
    }

    void noSecretOrUnknownResponseLogging()
    {
        Services services;
        SystemTab system(services);
        InteractionTab interaction(services);
        ModelTab model(services);
        SpeechTab speech(services);
        LogTab log(services);
        services.reply(QStringLiteral("set_speech_settings"), {{"ok", false}, {"api_key", "secret"}, {"error", "secret"}});
        services.reply(QStringLiteral("untrusted_transform_edit"), {{"ok", false}, {"api_key", "secret"}});
        QVERIFY(widget<QPlainTextEdit>(log, "responseLog")->toPlainText().isEmpty());
        services.reply(QStringLiteral("set_expression"), {{"ok", true}});
        QVERIFY(widget<QPlainTextEdit>(log, "responseLog")->toPlainText().contains(QStringLiteral("set_expression")));
    }

    void speechSaveClearsInputAndIgnoresOldQuery()
    {
        Services services;
        SpeechTab speech(services);
        speech.start();
        auto* key = widget<QLineEdit>(speech, "minimaxApiKey");
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        key->setText(QStringLiteral(" private-key "));
        widget<QPushButton>(speech, "saveSpeechKey")->click();
        QVERIFY(key->text().isEmpty());
        QCOMPARE(services.commands.last().second.value("api_key").toString(), QStringLiteral("private-key"));
        QVERIFY(!widget<QPushButton>(speech, "saveSpeechKey")->isEnabled());
        services.reply(QStringLiteral("get_speech_settings"), {{"ok", true}, {"configured", false}});
        QVERIFY(!widget<QPushButton>(speech, "saveSpeechKey")->isEnabled());
        services.reply(QStringLiteral("set_speech_settings"), {{"ok", true}, {"configured", true}});
        QVERIFY(widget<QPushButton>(speech, "clearSpeechKey")->isEnabled());
        services.disconnectBackend();
        QVERIFY(!widget<QPushButton>(speech, "saveSpeechKey")->isEnabled());
    }

    void hoverMutationCannotBeOverwrittenByQuery()
    {
        Services services;
        InteractionTab interaction(services);
        const QJsonObject initial{{"ok", true}, {"enabled", true}, {"opacity", 0.5}, {"idle_opacity", 1.0}};
        services.reply(QStringLiteral("get_hover_opacity"), initial);
        auto* slider = widget<QSlider>(interaction, "hoverOpacitySlider");
        slider->setValue(35);
        QCOMPARE(services.count(QStringLiteral("set_hover_opacity")), 1);
        services.reply(QStringLiteral("get_hover_opacity"), initial);
        QCOMPARE(slider->value(), 35);
        services.reply(QStringLiteral("set_hover_opacity"), {{"ok", false}});
        QCOMPARE(slider->value(), 50);
        QVERIFY(widget<QGroupBox>(interaction, "hoverOpacityGroup")->isEnabled());
    }

    void mouseLookFailureRestoresLastValue()
    {
        Services services;
        InteractionTab interaction(services);
        services.reply(QStringLiteral("get_mouse_look"), {{"ok", true}, {"radius", 4.5}});
        auto* spin = widget<QDoubleSpinBox>(interaction, "mouseLookRadiusSpin");
        QCOMPARE(spin->value(), 4.5);
        QCOMPARE(widget<QSlider>(interaction, "mouseLookRadiusSlider")->value(), 200);
        spin->setValue(1.25);
        services.reply(QStringLiteral("get_mouse_look"), {{"ok", true}, {"radius", 0.5}});
        QCOMPARE(spin->value(), 1.25);
        services.reply(QStringLiteral("set_mouse_look"), {{"ok", false}});
        QCOMPARE(spin->value(), 4.5);
    }

    void renderDeviceFailureRestoresSelection()
    {
        Services services;
        SystemTab system(services);
        services.reply(QStringLiteral("get_render_device"), {{"ok", true}, {"device", "auto"},
            {"devices", QJsonArray{}}, {"active_device", "auto"}, {"restart_required", false}});
        auto* combo = widget<QComboBox>(system, "renderDeviceCombo");
        QVERIFY(combo->isEnabled());
        combo->addItem(QStringLiteral("GPU"), QStringLiteral("0:0"));
        combo->setCurrentIndex(1);
        emit combo->activated(1);
        QVERIFY(!combo->isEnabled());
        services.reply(QStringLiteral("get_render_device"), {{"ok", true}, {"device", "auto"}});
        QCOMPARE(combo->currentIndex(), 1);
        services.reply(QStringLiteral("set_render_device"), {{"ok", false}});
        QCOMPARE(combo->currentData().toString(), QStringLiteral("auto"));
        QVERIFY(combo->isEnabled());
    }

    void modelRefreshIsIndependent()
    {
        Services services;
        ModelTab model(services);
        services.reply(QStringLiteral("get_current_model"), {{"ok", true}, {"model", "B"}});
        services.reply(QStringLiteral("get_available_models"), {{"ok", true}, {"models", QJsonArray{"A", "B"}}});
        QCOMPARE(widget<QComboBox>(model, "modelCombo")->currentText(), QStringLiteral("B"));
        services.commands.clear();
        services.reply(QStringLiteral("next_model"), {{"ok", true}});
        QCOMPARE(services.count(QStringLiteral("get_expressions")), 1);
        QCOMPARE(services.count(QStringLiteral("get_motions")), 1);
    }

    void transformPreviewAndSaveRecovery()
    {
        Services services;
        ModelTab model(services);
        QJsonObject transform{{"ok", true}, {"owned", false}, {"editing", false}, {"revision", 7},
            {"zoom", 1.0}, {"surface_width", 100}, {"surface_height", 100}, {"cropped", false}};
        services.reply(QStringLiteral("get_transform_settings"), transform);
        widget<QDoubleSpinBox>(model, "positionXSpin")->setValue(0.25);
        QCOMPARE(services.commands.last().first, QStringLiteral("begin_transform_edit"));
        QCOMPARE(services.commands.last().second.value("expected_revision").toInt(), 7);
        transform.insert("owned", true); transform.insert("editing", true);
        services.reply(QStringLiteral("begin_transform_edit"), transform);
        QCOMPARE(services.commands.last().first, QStringLiteral("update_transform_edit"));
        QCOMPARE(services.commands.last().second.value("x").toDouble(), 0.25);
        transform.insert("x", 0.25);
        services.reply(QStringLiteral("update_transform_edit"), transform);
        widget<QPushButton>(model, "transformButton")->click();
        QCOMPARE(services.commands.last().first, QStringLiteral("apply_transform_edit"));
        const auto request = services.commands.last().second.value("request_id").toString();
        QVERIFY(!request.isEmpty());
        services.disconnectBackend();
        services.connected = true;
        emit services.connectionChanged(true, QStringLiteral("online"));
        bool recovered = false;
        for (const auto& command : services.commands)
            if (command.first == QStringLiteral("get_transform_save_result") && command.second.value("request_id").toString() == request) recovered = true;
        QVERIFY(recovered);
    }
};
QTEST_MAIN(GuiTabsTest)
#include "GuiTabsTest.moc"
