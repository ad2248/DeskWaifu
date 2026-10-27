#include "SpeechTab.hpp"
#include "ui_SpeechTab.h"
#include "plugin/GuiHostServices.hpp"

SpeechTab::SpeechTab(GuiHostServices& services) : ui(new Ui::SpeechTab), _client(services)
{
    ui->setupUi(this);
    connect(ui->saveSpeechKey, &QPushButton::clicked, this, [this] {
        const auto key = ui->minimaxApiKey->text().trimmed();
        if (!key.isEmpty()) save(key);
    });
    connect(ui->clearSpeechKey, &QPushButton::clicked, this, [this] { save({}); });
    connect(&_client, &GuiHostServices::connectionChanged, this, [this](bool connected, const QString&) {
        ui->saveSpeechKey->setEnabled(connected);
        ui->clearSpeechKey->setEnabled(connected);
        if (!connected) {
            _speechSetPending = false;
            ui->speechSettingsStatus->setText(tr("未连接；未确认的保存结果将在重连后查询"));
        } else _client.sendCommand(QStringLiteral("get_speech_settings"));
    });
    connect(&_client, &GuiHostServices::responseReceived, this, &SpeechTab::handleResponse);
}
void SpeechTab::start()
{
    if (_client.isConnected()) {
        ui->saveSpeechKey->setEnabled(true);
        _client.sendCommand(QStringLiteral("get_speech_settings"));
    }
}
SpeechTab::~SpeechTab() { delete ui; }

void SpeechTab::save(const QString& key)
{
    if (!_client.isConnected() || _speechSetPending) return;
    _speechSetPending = true;
    ui->saveSpeechKey->setEnabled(false);
    ui->clearSpeechKey->setEnabled(false);
    _client.sendCommand(QStringLiteral("set_speech_settings"), {{QStringLiteral("api_key"), key}});
    ui->minimaxApiKey->clear();
    ui->speechSettingsStatus->setText(tr("正在保存…"));
}

void SpeechTab::handleResponse(const QString& command, const QJsonObject& response)
{
    if (command != QStringLiteral("get_speech_settings") && command != QStringLiteral("set_speech_settings")) return;
    const bool mutation = command == QStringLiteral("set_speech_settings");
    if (!mutation && _speechSetPending) return;
    if (mutation) _speechSetPending = false;
    const bool ok = response.value(QStringLiteral("ok")).toBool() && response.value(QStringLiteral("configured")).isBool();
    ui->speechSettingsStatus->setText(ok ? (response.value(QStringLiteral("configured")).toBool()
        ? tr("API Key 已保存；为安全起见不回显") : tr("未配置 API Key，语音功能不可用"))
        : tr("语音设置失败，请检查配置文件或后端版本"));
    ui->saveSpeechKey->setEnabled(_client.isConnected());
    ui->clearSpeechKey->setEnabled(_client.isConnected() && ok && response.value(QStringLiteral("configured")).toBool());
    if (mutation) _client.setStatusMessage(ui->speechSettingsStatus->text(), 5000);
    // Never log credential-related response payloads, including malformed responses.
}
