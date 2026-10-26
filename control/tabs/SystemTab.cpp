#include "SystemTab.hpp"
#include "ui_SystemTab.h"
#include "plugin/GuiHostServices.hpp"
#include "DeskWaifuLauncher.hpp"
#include "contracts/RenderDevice.hpp"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalBlocker>

SystemTab::SystemTab(GuiHostServices& services)
    : ui(new Ui::SystemTab), _client(services),
      _respawner(services, QCoreApplication::applicationDirPath(), this)
{
    ui->setupUi(this);
    _procDirectory = services.property("procDirectory").toString();
    if (_procDirectory.isEmpty()) _procDirectory = QStringLiteral("/proc");
    ui->renderDeviceCombo->setItemData(0, QStringLiteral("auto"));
    connect(ui->renderDeviceCombo, &QComboBox::activated, this, [this](int index) {
        if (!_client.isConnected() || _renderDeviceSetPending || index < 0) return;
        _renderDeviceSetPending = true;
        ui->renderDeviceCombo->setEnabled(false);
        ui->renderDeviceStatus->setText(tr("正在保存渲染设备…"));
        _client.sendCommand(QStringLiteral("set_render_device"), {
            {QStringLiteral("device"), ui->renderDeviceCombo->itemData(index).toString()}});
    });
    connect(ui->spawnButton, &QPushButton::clicked, this, [this] {
        if (_client.isConnected()) {
            _client.sendCommand(QStringLiteral("quit"));
            setStatusMessage(tr("已请求关闭 DeskWaifu。"), 8000);
            return;
        }
        const auto result = DeskWaifuLauncher::spawn(false, QCoreApplication::applicationDirPath(), _procDirectory);
        updateSpawnButton();
        setStatusMessage(result.message, 8000);
        _client.publishLog(result.message);
        if (result.state != DeskWaifuLauncher::State::Failed && !_client.isConnected()) _client.connectToServer();
    });
    connect(ui->respawnButton, &QPushButton::clicked, this, [this] { _respawner.start(_procDirectory); });
    connect(&_respawner, &DeskWaifuRespawner::runningChanged, this, [this](bool running) {
        updateSpawnButton();
        if (running) setStatusMessage(tr("正在重启 DeskWaifu，等待旧进程退出…"));
    });
    connect(&_respawner, &DeskWaifuRespawner::finished, this, [this](bool, const QString& message) {
        setStatusMessage(message, 8000);
        _client.publishLog(message);
    });
    _processTimer.setInterval(1000);
    connect(&_processTimer, &QTimer::timeout, this, &SystemTab::updateSpawnButton);
    _processTimer.start();
    connect(&_client, &GuiHostServices::connectionChanged, this, [this](bool connected, const QString&) {
        if (!connected) {
            _renderDeviceQueryPending = _renderDeviceSetPending = false;
            ui->renderDeviceCombo->setEnabled(false);
            ui->renderDeviceStatus->setText(tr("连接断开；重新连接后将查询实际保存结果。"));
        }
        updateSpawnButton();
        if (connected) refreshAll();
    });
    connect(&_client, &GuiHostServices::responseReceived, this, &SystemTab::handleResponse);
    updateSpawnButton();
}

void SystemTab::start() { if (_client.isConnected()) refreshAll(); }

SystemTab::~SystemTab()
{
    stop();
    delete ui;
}

void SystemTab::stop() noexcept
{
    _processTimer.stop();
    _respawner.stop();
}

void SystemTab::setStatusMessage(const QString& message, int timeout) { _client.setStatusMessage(message, timeout); }
void SystemTab::logResponse(const QString& command, const QJsonObject& response)
{
    _client.publishLog(QStringLiteral("%1  %2").arg(command, QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))));
}

void SystemTab::updateSpawnButton()
{
    const bool connected = _client.isConnected();
    const bool running = connected || DeskWaifuLauncher::runningPid(_procDirectory) > 0;
    ui->spawnButton->setText(running ? tr("关闭") : tr("启动"));
    ui->spawnButton->setEnabled(!_respawner.isRunning() && (!running || connected));
    ui->spawnButton->setToolTip(running && !connected ? tr("正在等待连接，以安全关闭 DeskWaifu。") : QString());
    ui->respawnButton->setEnabled(connected && !_respawner.isRunning());
}

void SystemTab::refreshAll()
{
    _client.sendCommand(QStringLiteral("get_status"));
    _client.sendCommand(QStringLiteral("get_current_model"));
    requestRenderDevice();
}

void SystemTab::handleResponse(const QString& command, const QJsonObject& response)
{
    if (command == QStringLiteral("get_render_device") || command == QStringLiteral("set_render_device")) {
        const bool mutation = command == QStringLiteral("set_render_device");
        if (mutation) _renderDeviceSetPending = false;
        else _renderDeviceQueryPending = false;
        if (!mutation && _renderDeviceSetPending) return;
        const auto device = response.value(QStringLiteral("device"));
        const bool valid = device.isString() && ValidRenderDevice(device.toString().toStdString()) &&
            response.value(QStringLiteral("devices")).isArray() && response.value(QStringLiteral("active_device")).isString() &&
            ValidRenderDevice(response.value(QStringLiteral("active_device")).toString().toStdString()) &&
            response.value(QStringLiteral("restart_required")).isBool();
        if (valid) { _lastRenderDeviceResponse = response; showRenderDevice(response); }
        else if (mutation && !_lastRenderDeviceResponse.isEmpty()) showRenderDevice(_lastRenderDeviceResponse);
        ui->renderDeviceCombo->setEnabled(_client.isConnected() && (valid || (mutation && !_lastRenderDeviceResponse.isEmpty())));
        const bool ok = response.value(QStringLiteral("ok")).toBool() && valid;
        if (!ok) ui->renderDeviceStatus->setText(response.value(QStringLiteral("error")).toString(tr("后端不支持渲染设备设置")));
        if (mutation) setStatusMessage(ok ? tr("渲染设备已保存；点击“重启”生效。") : ui->renderDeviceStatus->text(), 8000);
        if (mutation || !ok) logResponse(command, response);
        return;
    }
    if (command == QStringLiteral("quit")) {
        logResponse(command, response);
        return;
    }
    const bool modelChanged = command == QStringLiteral("set_model") || command == QStringLiteral("next_model") || command == QStringLiteral("prev_model");
    if (modelChanged) { if (response.value("ok").toBool()) refreshAll(); return; }
    if (command != QStringLiteral("get_status") && command != QStringLiteral("get_current_model") && command != QStringLiteral("toggle_hidden")) return;
    if (command != QStringLiteral("toggle_hidden")) logResponse(command, response);
    if (!response.value(QStringLiteral("ok")).toBool()) {
        setStatusMessage(response.value(QStringLiteral("error")).toString(), 5000);
        return;
    }
    if (command == QStringLiteral("get_status")) {
        ui->currentModelLabel->setText(response.value(QStringLiteral("model")).toString());
        ui->modelCountLabel->setText(QString::number(response.value(QStringLiteral("model_count")).toInt()));
        ui->visibilityLabel->setText(response.value(QStringLiteral("hidden")).toBool() ? tr("已隐藏") : tr("已显示"));
        _client.notifyBackendReady();
    } else if (command == QStringLiteral("get_current_model")) {
        ui->currentModelLabel->setText(response.value(QStringLiteral("model")).toString());
    } else {
        ui->visibilityLabel->setText(response.value(QStringLiteral("hidden")).toBool() ? tr("已隐藏") : tr("已显示"));
    }
}

void SystemTab::requestRenderDevice()
{
    if (_renderDeviceQueryPending || _renderDeviceSetPending) return;
    _renderDeviceQueryPending = true;
    _client.sendCommand(QStringLiteral("get_render_device"));
}

void SystemTab::showRenderDevice(const QJsonObject& response)
{
    const QSignalBlocker blocker(ui->renderDeviceCombo);
    ui->renderDeviceCombo->clear();
    ui->renderDeviceCombo->addItem(tr("自动"), QStringLiteral("auto"));
    for (const auto& value : response.value(QStringLiteral("devices")).toArray()) {
        const auto device = value.toObject();
        const auto id = device.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id == QStringLiteral("auto") || !ValidRenderDevice(id.toStdString()) || ui->renderDeviceCombo->findData(id) >= 0) continue;
        ui->renderDeviceCombo->addItem(QStringLiteral("%1 (%2)").arg(device.value(QStringLiteral("name")).toString(), id), id);
    }
    auto saved = response.value(QStringLiteral("device")).toString();
    if (saved.isEmpty()) saved = QStringLiteral("auto");
    if (ui->renderDeviceCombo->findData(saved) < 0) ui->renderDeviceCombo->addItem(tr("%1（不可用）").arg(saved), saved);
    ui->renderDeviceCombo->setCurrentIndex(ui->renderDeviceCombo->findData(saved));
    const auto active = response.value(QStringLiteral("active_device")).toString();
    ui->renderDeviceStatus->setText(response.value(QStringLiteral("restart_required")).toBool()
        ? tr("已保存，需点击“重启”生效。当前启动选择：%1").arg(active == QStringLiteral("auto") ? tr("自动") : active)
        : tr("已保存；更改渲染设备后需重启生效。"));
}
