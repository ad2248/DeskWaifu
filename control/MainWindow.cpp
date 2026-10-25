#include "MainWindow.hpp"
#include "ui_MainWindow.h"
#include "HotkeyPanel.hpp"
#include "DeskWaifuLauncher.hpp"
#include "RenderDevice.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <QUuid>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QVariantMap>
#include <QFormLayout>
#include <QLineEdit>

MainWindow::MainWindow(QWidget* parent, const QString& procDirectory)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , _procDirectory(procDirectory)
    , _respawner(_client, QCoreApplication::applicationDirPath())
{
    ui->setupUi(this);
    ui->renderDeviceCombo->setItemData(0, QStringLiteral("auto"));
    setupSpeechSettings();
    _connectionStatusLabel = new QLabel(tr("尚未连接"), this);
    _connectionStatusLabel->setObjectName(QStringLiteral("connectionStatusLabel"));
    _connectionStatusLabel->setTextFormat(Qt::PlainText);
    _connectionStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(_connectionStatusLabel, 1);
    _operationStatusLabel = new QLabel(this);
    _operationStatusLabel->setObjectName(QStringLiteral("operationStatusLabel"));
    _operationStatusLabel->setTextFormat(Qt::PlainText);
    _operationStatusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    _operationStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addPermanentWidget(_operationStatusLabel, 1);
    _statusMessageTimer.setSingleShot(true);
    connect(&_statusMessageTimer, &QTimer::timeout, this, [this] { setStatusMessage(QString()); });
    _hotkeys = new HotkeyPanel(this);
    ui->tabWidget->insertTab(2, _hotkeys, tr("Hotkeys"));
    connect(_hotkeys, &HotkeyPanel::triggered, this, [this](const QString& id) {
        if (_client.isConnected() && !_hotkeyModelToken.isEmpty())
            _client.sendCommand(QStringLiteral("trigger_hotkey"), {
                {QStringLiteral("id"), id}, {QStringLiteral("model_token"), _hotkeyModelToken}});
    });
    auto* hotkeyTimer = new QTimer(this);
    hotkeyTimer->setInterval(1500);
    connect(hotkeyTimer, &QTimer::timeout, this, [this] {
        if (_client.isConnected() && isActiveWindow()) {
            requestHotkeys();
            requestHoverOpacity();
            requestMouseLook();
        }
    });
    hotkeyTimer->start();
    auto* transformTimer = new QTimer(this);
    transformTimer->setInterval(200);
    connect(transformTimer, &QTimer::timeout, this, [this] {
        if (_client.isConnected() && _transformSupported && isActiveWindow()) {
            if (_transformPreviewPending) queueTransformPreview();
            else requestTransform();
        }
    });
    transformTimer->start();
    bindControls();

    connect(&_client, &DeskWaifuClient::connectionChanged, this,
        [this](bool connected, const QString& message) {
            _hotkeys->setConnected(connected);
            _speechSave->setEnabled(connected);
            _speechClear->setEnabled(connected);
            if (!connected) {
                _renderDeviceQueryPending = _renderDeviceSetPending = false;
                ui->renderDeviceCombo->setEnabled(false);
                ui->renderDeviceStatus->setText(tr("连接断开；重新连接后将查询实际保存结果。"));
                _speechSetPending = false;
                _speechStatus->setText(tr("未连接；未确认的保存结果将在重连后查询"));
            }
            ui->respawnButton->setEnabled(connected && !_respawner.isRunning());
            updateSpawnButton();
            if (!connected)
            {
                if (_hoverSetPending || _mouseLookSetPending) setStatusMessage(tr("连接断开：设置结果未确认，重新连接后将查询实际保存结果。"));
                _transformOwned = _transformEditStarting = _transformQueryPending = false;
                _transformPreviewInFlight = _transformPreviewPending = _transformSupported = false;
                _outputSwitchPending = false;
                ui->transformStatusLabel->setText(tr("连接断开：未确认的编辑已取消，请重新查询保存结果。"));
                _hotkeyModelToken.clear();
                _lastHotkeyResponse = {};
                _hotkeyQueryPending = false;
                _hoverQueryPending = _hoverSetPending = _hoverSupported = false;
                _mouseLookQueryPending = _mouseLookSetPending = _mouseLookSupported = false;
            }
            _connectionStatusLabel->setText(message);
            _connectionStatusLabel->setToolTip(message);
            ui->hoverOpacityGroup->setEnabled(false);
            ui->mouseLookGroup->setEnabled(false);
            ui->transformGroup->setEnabled(connected && _transformSupported);
            ui->outputCombo->setEnabled(false);
            ui->positionEdit->setEnabled(connected && _transformSupported);
            ui->xformResetButton->setEnabled(connected && _transformSupported);
            ui->cancelTransformButton->setEnabled(false);
            ui->relocateCropButton->setEnabled(false);
            ui->retryTransformButton->setEnabled(false);
            if (connected)
            {
                if (!_transformSaveRequest.isEmpty()) _client.sendCommand(QStringLiteral("get_transform_save_result"),
                    {{QStringLiteral("request_id"), _transformSaveRequest}});
                refreshAll();
            }
        });
    connect(&_client, &DeskWaifuClient::responseReceived,
        this, &MainWindow::handleResponse);
    connect(&_client, &DeskWaifuClient::protocolError, this,
        [this](const QString& message) {
            ui->responseLog->appendPlainText(message);
            setStatusMessage(message, 5000);
        });
    connect(&_client, &DeskWaifuClient::commandOutcomeUnknown, this,
        [this](const QString& command) {
            const QString message = tr("Connection lost: outcome of '%1' is unknown; command was not retried.")
                .arg(command);
            ui->responseLog->appendPlainText(message);
            setStatusMessage(message, 8000);
        });
    connect(&_client, &DeskWaifuClient::commandCancelled, this,
        [this](const QString& command) {
            if (command == QStringLiteral("apply_transform_edit") ||
                command == QStringLiteral("reset_transform_settings") ||
                command == QStringLiteral("retry_transform_save")) _transformSaveRequest.clear();
            const QString message = tr("Connection lost: queued command '%1' was cancelled before sending.")
                .arg(command);
            ui->responseLog->appendPlainText(message);
            setStatusMessage(message, 8000);
        });

    _client.connectToServer();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setupSpeechSettings()
{
    auto* tab = new QWidget(this);
    auto* layout = new QFormLayout(tab);
    _speechKey = new QLineEdit(tab);
    _speechKey->setObjectName(QStringLiteral("minimaxApiKey"));
    _speechKey->setEchoMode(QLineEdit::Password);
    _speechKey->setMaxLength(4096);
    _speechKey->setPlaceholderText(tr("输入新的 MiniMax API Key"));
    _speechStatus = new QLabel(tr("尚未查询"), tab);
    _speechStatus->setObjectName(QStringLiteral("speechSettingsStatus"));
    _speechSave = new QPushButton(tr("保存 API Key"), tab);
    _speechSave->setObjectName(QStringLiteral("saveSpeechKey"));
    _speechClear = new QPushButton(tr("删除已保存的 API Key"), tab);
    _speechClear->setObjectName(QStringLiteral("clearSpeechKey"));
    _speechSave->setEnabled(false);
    _speechClear->setEnabled(false);
    layout->addRow(tr("MiniMax API Key"), _speechKey);
    layout->addRow(_speechStatus);
    layout->addRow(_speechSave);
    layout->addRow(_speechClear);
    auto* notice = new QLabel(tr("密钥以明文保存到桌宠配置文件（权限 0600），不会回显。\n"
        "双击模型播报“姐姐，你好喵”；再次双击会打断重播。\n"
        "播报文本将发送到 MiniMax，可能产生 API 费用。"), tab);
    notice->setWordWrap(true);
    layout->addRow(notice);
    auto save = [this](const QString& key) {
        if (!_client.isConnected() || _speechSetPending) return;
        _speechSetPending = true;
        _speechSave->setEnabled(false);
        _speechClear->setEnabled(false);
        _client.sendCommand(QStringLiteral("set_speech_settings"), {{QStringLiteral("api_key"), key}});
        _speechKey->clear();
        _speechStatus->setText(tr("正在保存…"));
    };
    connect(_speechSave, &QPushButton::clicked, this, [this, save] {
        if (!_speechKey->text().trimmed().isEmpty()) save(_speechKey->text().trimmed());
    });
    connect(_speechClear, &QPushButton::clicked, this, [save] { save({}); });
    ui->tabWidget->addTab(tab, tr("语音"));
}

void MainWindow::setStatusMessage(const QString& message, int timeoutMs)
{
    _statusMessageTimer.stop();
    _operationStatusLabel->setText(message);
    _operationStatusLabel->setToolTip(message);
    if (!message.isEmpty() && timeoutMs > 0) _statusMessageTimer.start(timeoutMs);
}

void MainWindow::updateSpawnButton()
{
    const bool connected = _client.isConnected();
    const bool running = connected || DeskWaifuLauncher::runningPid(_procDirectory) > 0;
    ui->spawnButton->setText(running ? tr("Shutdown") : tr("Spawn"));
    ui->spawnButton->setEnabled(!_respawner.isRunning() && (!running || connected));
    ui->spawnButton->setToolTip(running && !connected
        ? tr("Waiting for a connection to shut down DeskWaifu safely.") : QString());
}

void MainWindow::bindControls()
{
    connect(ui->renderDeviceCombo, &QComboBox::activated, this, [this](int index) {
        if (!_client.isConnected() || _renderDeviceSetPending || index < 0) return;
        _renderDeviceSetPending = true;
        ui->renderDeviceCombo->setEnabled(false);
        ui->renderDeviceStatus->setText(tr("正在保存渲染设备…"));
        _client.sendCommand(QStringLiteral("set_render_device"), {
            {QStringLiteral("device"), ui->renderDeviceCombo->itemData(index).toString()}});
    });
    connect(ui->outputCombo, &QComboBox::activated, this, [this](int index) {
        if (_outputSwitchPending || index < 0) return;
        _outputSwitchPending = true;
        ui->outputCombo->setEnabled(false);
        ui->transformGroup->setEnabled(false);
        ui->positionEdit->setEnabled(false);
        _client.sendCommand(QStringLiteral("set_output"), {{QStringLiteral("name"), ui->outputCombo->itemData(index).toString()}});
    });
    ui->horizontalLayout->setColumnStretch(1, 1);
    ui->horizontalLayout->setColumnStretch(3, 1);
    ui->horizontalLayout_2->setColumnStretch(1, 1);
    ui->horizontalLayout_2->setColumnStretch(3, 1);
    ui->respawnButton->setEnabled(false);
    auto* processTimer = new QTimer(this);
    processTimer->setInterval(1000);
    connect(processTimer, &QTimer::timeout, this, &MainWindow::updateSpawnButton);
    processTimer->start();
    updateSpawnButton();
    connect(&_respawner, &DeskWaifuRespawner::runningChanged, this, [this](bool running) {
        updateSpawnButton();
        ui->respawnButton->setEnabled(!running && _client.isConnected());
        if (running) setStatusMessage(tr("Respawning DeskWaifu; waiting for the old process to exit…"));
    });
    connect(&_respawner, &DeskWaifuRespawner::finished, this, [this](bool, const QString& message) {
        setStatusMessage(message, 8000);
        ui->responseLog->appendPlainText(message);
    });
    connect(ui->respawnButton, &QPushButton::clicked, this, [this] { _respawner.start(); });
    connect(ui->spawnButton, &QPushButton::clicked, this, [this] {
        if (_client.isConnected()) {
            _client.sendCommand(QStringLiteral("quit"));
            setStatusMessage(tr("Requested DeskWaifu shutdown."), 8000);
            return;
        }
        const auto result = DeskWaifuLauncher::spawn(_client.isConnected(), QCoreApplication::applicationDirPath(), _procDirectory);
        updateSpawnButton();
        setStatusMessage(result.message, 8000);
        ui->responseLog->appendPlainText(result.message);
        if (result.state != DeskWaifuLauncher::State::Failed && !_client.isConnected())
            _client.connectToServer();
    });
    connect(ui->previousModelButton, &QPushButton::clicked, this,
        [this] { _client.sendCommand(QStringLiteral("prev_model")); });
    connect(ui->nextModelButton, &QPushButton::clicked, this,
        [this] { _client.sendCommand(QStringLiteral("next_model")); });
    connect(ui->setModelButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("set_model"),
            {{QStringLiteral("name"), ui->modelCombo->currentText()}});
    });
    connect(ui->setExpressionButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("set_expression"),
            {{QStringLiteral("id"), ui->expressionCombo->currentText()}});
    });
    connect(ui->playMotionButton, &QPushButton::clicked, this, [this] {
        const QVariantMap motion = ui->motionCombo->currentData().toMap();
        _client.sendCommand(QStringLiteral("do_motion"), {
            {QStringLiteral("group"), motion.value(QStringLiteral("group")).toString()},
            {QStringLiteral("index"), motion.value(QStringLiteral("index")).toInt()},
            {QStringLiteral("priority"), ui->prioritySpin->value()}
        });
    });
    connect(ui->randomMotionButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("do_motion"),
            {{QStringLiteral("priority"), ui->prioritySpin->value()}});
    });
    connect(ui->mouthButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("set_mouth_y"),
            {{QStringLiteral("value"), ui->mouthSpin->value()}});
    });
    connect(ui->transformButton, &QPushButton::clicked, this, [this] {
        if (!_transformOwned || _transformPreviewPending || _transformPreviewInFlight) return;
        bool overwrite = false;
        if (_transformConflict) {
            const auto answer = QMessageBox::question(this, tr("配置冲突"),
                tr("配置文件已在外部修改。是否覆盖位置、缩放和裁剪设置？选择否将放弃草稿并重新载入。"),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer == QMessageBox::Cancel) return;
            if (answer == QMessageBox::No) { _client.sendCommand(QStringLiteral("reload_transform_settings")); return; }
            overwrite = true;
        }
        sendTransformSave(QStringLiteral("apply_transform_edit"), {{QStringLiteral("overwrite"), overwrite}});
    });
    connect(ui->dragEditButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("begin_transform_edit"), {{QStringLiteral("screen"), true},
            {QStringLiteral("expected_revision"), _lastTransformResponse.value(QStringLiteral("revision"))}});
    });
    connect(ui->cancelTransformButton, &QPushButton::clicked, this, [this] {
        _transformPreviewPending = false;
        _client.sendCommand(QStringLiteral("cancel_transform_edit"));
    });
    connect(ui->relocateCropButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("relocate_crop"));
    });
    connect(ui->retryTransformButton, &QPushButton::clicked, this, [this] {
        if (_transformConflict) {
            const auto answer = QMessageBox::question(this, tr("配置冲突"),
                tr("是否放弃未保存修改并重新载入配置文件？选择否可通过编辑、应用明确覆盖文件。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes) _client.sendCommand(QStringLiteral("reload_transform_settings"));
            return;
        }
        sendTransformSave(QStringLiteral("retry_transform_save"));
    });
    connect(ui->xformResetButton, &QPushButton::clicked, this, [this] {
        bool overwrite = false;
        if (_transformConflict) {
            if (QMessageBox::question(this, tr("配置冲突"), tr("是否覆盖外部修改，将位置、缩放和裁剪恢复默认？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            overwrite = true;
        }
        _transformPreviewPending = false;
        sendTransformSave(QStringLiteral("reset_transform_settings"), {{QStringLiteral("overwrite"), overwrite}});
    });
    for (auto* spin : {ui->positionXSpin, ui->positionYSpin, ui->zoomSpin}) {
        spin->setDecimals(6);
        spin->setKeyboardTracking(false);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { queueTransformPreview(); });
    }
    ui->positionXSpin->setRange(-1.e30, 1.e30);
    ui->positionYSpin->setRange(-1.e30, 1.e30);
    for (auto* spin : {ui->cropBox_x, ui->cropBox_y, ui->cropBox_w, ui->cropBox_h}) {
        spin->setSuffix(tr(" px"));
        spin->setKeyboardTracking(false);
        connect(spin, &QSpinBox::valueChanged, this, [this] { queueTransformPreview(); });
    }
    connect(ui->cropEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        for (auto* spin : {ui->cropBox_x, ui->cropBox_y, ui->cropBox_w, ui->cropBox_h}) spin->setEnabled(enabled);
        if (_transformUpdating) return;
        if (enabled && !_lastTransformResponse.value(QStringLiteral("cropped")).toBool()) {
            _transformUpdating = true;
            ui->cropBox_x->setValue(0); ui->cropBox_y->setValue(0);
            ui->cropBox_w->setValue(_surfaceWidth); ui->cropBox_h->setValue(_surfaceHeight);
            _transformUpdating = false;
        }
        queueTransformPreview();
    });
    ui->transformGroup->setEnabled(false); ui->positionEdit->setEnabled(false);
    ui->outputCombo->setEnabled(false);
    ui->xformResetButton->setEnabled(false); ui->cancelTransformButton->setEnabled(false);
    ui->relocateCropButton->setEnabled(false); ui->retryTransformButton->setEnabled(false);
    connect(ui->hoverOpacitySlider, &QSlider::sliderMoved, this, [this](int value) {
        ui->hoverOpacityValueLabel->setText(QStringLiteral("%1%").arg(value));
    });
    connect(ui->hoverOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        ui->hoverOpacityValueLabel->setText(QStringLiteral("%1%").arg(value));
        setHoverOpacity({{QStringLiteral("opacity"), value / 100.0}});
    });
    connect(ui->hoverOpacityEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        setHoverOpacity({{QStringLiteral("enabled"), enabled}});
    });
    connect(ui->idleOpacitySlider, &QSlider::sliderMoved, this, [this](int value) {
        ui->idleOpacityValueLabel->setText(QStringLiteral("%1%").arg(value));
    });
    connect(ui->idleOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        ui->idleOpacityValueLabel->setText(QStringLiteral("%1%").arg(value));
        setHoverOpacity({{QStringLiteral("idle_opacity"), value / 100.0}});
    });
    connect(ui->mouseLookRadiusSlider, &QSlider::sliderMoved, this, [this](int value) {
        const QSignalBlocker block(ui->mouseLookRadiusSpin);
        ui->mouseLookRadiusSpin->setValue(value / 100.0);
    });
    connect(ui->mouseLookRadiusSlider, &QSlider::valueChanged, this, [this](int value) {
        showMouseLook(value / 100.0);
        setMouseLook(value / 100.0);
    });
    ui->mouseLookRadiusSpin->setKeyboardTracking(false);
    connect(ui->mouseLookRadiusSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        showMouseLook(value);
        setMouseLook(value);
    });
    connect(ui->lookButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("set_look"), {
            {QStringLiteral("x"), ui->lookXSpin->value()},
            {QStringLiteral("y"), ui->lookYSpin->value()}
        });
    });
    connect(ui->resetLookButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("set_look"), {{QStringLiteral("reset"), true}});
    });
    connect(ui->toggleHiddenButton, &QPushButton::clicked, this,
        [this] { _client.sendCommand(QStringLiteral("toggle_hidden")); });
    connect(ui->switchSkinButton, &QPushButton::clicked, this,
        [this] { _client.sendCommand(QStringLiteral("switch_skin")); });
}

void MainWindow::refreshAll()
{
    _client.sendCommand(QStringLiteral("get_status"));
    _client.sendCommand(QStringLiteral("get_current_model"));
    _client.sendCommand(QStringLiteral("get_available_models"));
    _client.sendCommand(QStringLiteral("get_expressions"));
    requestHotkeys();
    _client.sendCommand(QStringLiteral("get_motions"));
    requestTransform();
    requestHoverOpacity();
    requestMouseLook();
    requestRenderDevice();
    _client.sendCommand(QStringLiteral("get_speech_settings"));
}

void MainWindow::handleResponse(const QString& command, const QJsonObject& response)
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
        if (valid) {
            _lastRenderDeviceResponse = response;
            showRenderDevice(response);
        } else if (mutation && !_lastRenderDeviceResponse.isEmpty()) showRenderDevice(_lastRenderDeviceResponse);
        ui->renderDeviceCombo->setEnabled(_client.isConnected() && (valid || (mutation && !_lastRenderDeviceResponse.isEmpty())));
        const bool ok = response.value(QStringLiteral("ok")).toBool() && valid;
        if (!ok) ui->renderDeviceStatus->setText(response.value(QStringLiteral("error")).toString(tr("后端不支持渲染设备设置")));
        if (mutation) setStatusMessage(ok ? tr("渲染设备已保存；点击 Respawn 重启生效。") : ui->renderDeviceStatus->text(), 8000);
        if (mutation || !ok) logResponse(command, response);
        return;
    }
    if (command == QStringLiteral("get_speech_settings") || command == QStringLiteral("set_speech_settings")) {
        const bool mutation = command == QStringLiteral("set_speech_settings");
        if (!mutation && _speechSetPending) return;
        if (mutation) _speechSetPending = false;
        const bool ok = response.value(QStringLiteral("ok")).toBool() && response.value(QStringLiteral("configured")).isBool();
        _speechStatus->setText(ok ? (response.value(QStringLiteral("configured")).toBool()
            ? tr("API Key 已保存；为安全起见不回显") : tr("未配置 API Key，语音功能不可用"))
            : tr("语音设置失败，请检查配置文件或后端版本"));
        _speechSave->setEnabled(_client.isConnected());
        _speechClear->setEnabled(_client.isConnected() && ok && response.value(QStringLiteral("configured")).toBool());
        if (mutation) setStatusMessage(_speechStatus->text(), 5000);
        return; // 不能将凭据相关内容写入回复日志。
    }
    if (command == QStringLiteral("get_mouse_look") || command == QStringLiteral("set_mouse_look")) {
        const bool mutation = command == QStringLiteral("set_mouse_look");
        if (mutation) _mouseLookSetPending = false;
        else _mouseLookQueryPending = false;
        if (!mutation && (_mouseLookSetPending || ui->mouseLookRadiusSlider->isSliderDown() ||
            ui->mouseLookRadiusSpin->hasFocus())) return;
        const auto value = response.value(QStringLiteral("radius"));
        const double radius = value.toDouble(-1);
        const bool valid = value.isDouble() && std::isfinite(radius) && radius >= 0;
        if (valid) {
            _mouseLookSupported = true;
            _lastMouseLookRadius = radius;
        } else if (!mutation) _mouseLookSupported = false;
        showMouseLook(_lastMouseLookRadius);
        ui->mouseLookGroup->setEnabled(_client.isConnected() && _mouseLookSupported);
        const bool ok = response.value(QStringLiteral("ok")).toBool() && valid;
        if (ok && mutation) setStatusMessage(tr("已设置并保存"));
        ui->mouseLookGroup->setToolTip(ok ? QString() : response.value(QStringLiteral("error")).toString(tr("后端不支持鼠标注视范围设置")));
        if (!ok && (mutation || valid)) setStatusMessage(ui->mouseLookGroup->toolTip());
        if (mutation || !ok) logResponse(command, response);
        return;
    }
    if (command == QStringLiteral("get_hover_opacity") || command == QStringLiteral("set_hover_opacity")) {
        const bool mutation = command == QStringLiteral("set_hover_opacity");
        if (mutation) _hoverSetPending = false;
        else _hoverQueryPending = false;
        // 用户释放鼠标前发出的查询，不能覆盖其拖动位置，
        // 也不能覆盖排在该查询之后的修改操作。
        if (!mutation && (_hoverSetPending || ui->hoverOpacitySlider->isSliderDown() || ui->idleOpacitySlider->isSliderDown())) return;
        const double opacity = response.value(QStringLiteral("opacity")).toDouble(-1);
        const auto idleOpacity = response.value(QStringLiteral("idle_opacity"));
        const bool valid = response.value(QStringLiteral("enabled")).isBool()
            && response.value(QStringLiteral("opacity")).isDouble()
            && std::isfinite(opacity) && opacity >= 0 && opacity <= 1
            && (idleOpacity.isUndefined() || (idleOpacity.isDouble() && std::isfinite(idleOpacity.toDouble())
                && idleOpacity.toDouble() >= 0 && idleOpacity.toDouble() <= 1));
        if (valid) {
            _hoverSupported = true;
            _lastHoverResponse = response;
            showHoverOpacity(response);
        } else {
            if (mutation && !_lastHoverResponse.isEmpty()) showHoverOpacity(_lastHoverResponse);
            else _hoverSupported = false;
        }
        ui->hoverOpacityGroup->setEnabled(_client.isConnected() && _hoverSupported);
        ui->idleOpacitySlider->setEnabled(response.value(QStringLiteral("idle_opacity")).isDouble());
        const bool ok = response.value(QStringLiteral("ok")).toBool() && valid;
        if (ok && mutation) setStatusMessage(tr("已设置并保存"));
        if (!ok) setStatusMessage(response.value(QStringLiteral("error")).toString(tr("后端不支持悬停透明度设置")));
        if (mutation || !ok) logResponse(command, response);
        return;
    }
    if (command == QStringLiteral("get_transform_save_result")) {
        const QString message = !response.value(QStringLiteral("known")).toBool() && response.value(QStringLiteral("ok")).toBool()
            ? tr("无法确认上次保存结果（后端可能已重启），请核对当前配置。")
            : response.value(QStringLiteral("ok")).toBool() ? tr("上次保存已成功。") : response.value(QStringLiteral("error")).toString();
        ui->responseLog->appendPlainText(message);
        setStatusMessage(message, 10000);
        _transformSaveRequest.clear();
        requestTransform();
        return;
    }
    if (command == QStringLiteral("apply_transform_edit") || command == QStringLiteral("reset_transform_settings") || command == QStringLiteral("retry_transform_save")) {
        _transformSaveRequest.clear();
        if (response.value(QStringLiteral("ok")).toBool()) setStatusMessage(tr("变换设置已保存"));
    }
    if (command == QStringLiteral("set_output")) {
        _outputSwitchPending = false;
        if (response.value(QStringLiteral("ok")).toBool()) setStatusMessage(tr("显示器已保存，模型已居中并清除裁剪"));
        requestTransform();
    }
    const bool transformResponse = command == QStringLiteral("get_transform_settings")
        || command.endsWith(QStringLiteral("transform_edit")) || command == QStringLiteral("reset_transform_settings")
        || command == QStringLiteral("relocate_crop") || command == QStringLiteral("retry_transform_save")
        || command == QStringLiteral("reload_transform_settings");
    if (command == QStringLiteral("get_transform_settings")) _transformQueryPending = false;
    if (command == QStringLiteral("begin_transform_edit")) _transformEditStarting = false;
    if (command == QStringLiteral("update_transform_edit")) _transformPreviewInFlight = false;
    if (transformResponse) {
        if (response.value(QStringLiteral("ok")).toBool()) {
            _transformSupported = true;
            _transformOwned = response.value(QStringLiteral("owned")).toBool();
            _transformConflict = response.value(QStringLiteral("conflict")).toBool();
            if (!_transformPreviewPending && !_transformPreviewInFlight && !_transformEditStarting) showTransform(response);
            if (_transformPreviewPending) queueTransformPreview();
        } else {
            if (command == QStringLiteral("begin_transform_edit")) _transformPreviewPending = false;
            if (command != QStringLiteral("get_transform_settings")) requestTransform();
        }
    }
    if (command != QStringLiteral("get_transform_settings") && (command != QStringLiteral("get_hotkeys") || response != _lastHotkeyResponse))
        logResponse(command, response);
    if (command == QStringLiteral("get_hotkeys") || command == QStringLiteral("trigger_hotkey"))
    {
        if (command == QStringLiteral("get_hotkeys")) _hotkeyQueryPending = false;
        if (response.value(QStringLiteral("ok")).toBool())
        {
            if (response != _lastHotkeyResponse)
            {
                if (_hotkeyModelToken != response.value(QStringLiteral("model_token")).toString())
                    _hotkeys->resetInput();
                _hotkeys->setBindings(response.value(QStringLiteral("hotkeys")).toArray(), response.value(QStringLiteral("warning")).toString());
                _hotkeyModelToken = response.value(QStringLiteral("model_token")).toString();
                _lastHotkeyResponse = response;
            }
        }
        else if (command == QStringLiteral("get_hotkeys"))
        {
            _hotkeyModelToken.clear();
            _lastHotkeyResponse = {};
            _hotkeys->setBindings({}, response.value(QStringLiteral("error")).toString());
        }
        else requestHotkeys();
    }
    if (!response.value(QStringLiteral("ok")).toBool())
    {
        setStatusMessage(response.value(QStringLiteral("error")).toString(), 5000);
        return;
    }

    if (command == QStringLiteral("get_status"))
    {
        ui->currentModelLabel->setText(response.value(QStringLiteral("model")).toString());
        ui->modelCountLabel->setText(QString::number(response.value(QStringLiteral("model_count")).toInt()));
        ui->visibilityLabel->setText(response.value(QStringLiteral("hidden")).toBool() ? tr("Hidden") : tr("Visible"));
        if (!_backendReadyEmitted)
        {
            _backendReadyEmitted = true;
            emit backendReady();
        }
    }
    else if (command == QStringLiteral("get_current_model"))
    {
        ui->currentModelLabel->setText(response.value(QStringLiteral("model")).toString());
    }
    else if (command == QStringLiteral("get_available_models"))
    {
        const QString current = ui->currentModelLabel->text();
        ui->modelCombo->clear();
        for (const QJsonValue& value : response.value(QStringLiteral("models")).toArray())
        {
            ui->modelCombo->addItem(value.toString());
        }
        ui->modelCombo->setCurrentText(current);
    }
    else if (command == QStringLiteral("get_expressions"))
    {
        ui->expressionCombo->clear();
        for (const QJsonValue& value : response.value(QStringLiteral("expressions")).toArray())
        {
            ui->expressionCombo->addItem(value.toString());
        }
    }
    else if (command == QStringLiteral("get_motions"))
    {
        ui->motionCombo->clear();
        for (const QJsonValue& value : response.value(QStringLiteral("motions")).toArray())
        {
            const QJsonObject motion = value.toObject();
            const QString group = motion.value(QStringLiteral("group")).toString();
            const int index = motion.value(QStringLiteral("index")).toInt();
            const QString file = motion.value(QStringLiteral("file")).toString();
            ui->motionCombo->addItem(QStringLiteral("%1 / %2 - %3").arg(group).arg(index).arg(file),
                QVariantMap{{QStringLiteral("group"), group}, {QStringLiteral("index"), index}});
        }
    }
    else if (command == QStringLiteral("get_model_position"))
    {
        ui->positionXSpin->setValue(response.value(QStringLiteral("x")).toDouble());
        ui->positionYSpin->setValue(response.value(QStringLiteral("y")).toDouble());
        ui->zoomSpin->setValue(response.value(QStringLiteral("zoom")).toDouble());
    }
    else if (command == QStringLiteral("toggle_hidden"))
    {
        ui->visibilityLabel->setText(response.value(QStringLiteral("hidden")).toBool() ? tr("Hidden") : tr("Visible"));
    }

    if (command == QStringLiteral("set_model") || command == QStringLiteral("next_model")
        || command == QStringLiteral("prev_model"))
    {
        refreshAll();
    }
}

void MainWindow::logResponse(const QString& command, const QJsonObject& response)
{
    const QString json = QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact));
    ui->responseLog->appendPlainText(QStringLiteral("%1  %2").arg(command, json));
}

void MainWindow::requestHotkeys()
{
    if (_hotkeyQueryPending) return;
    _hotkeyQueryPending = true;
    _client.sendCommand(QStringLiteral("get_hotkeys"));
}

void MainWindow::requestRenderDevice()
{
    if (_renderDeviceQueryPending || _renderDeviceSetPending) return;
    _renderDeviceQueryPending = true;
    _client.sendCommand(QStringLiteral("get_render_device"));
}

void MainWindow::showRenderDevice(const QJsonObject& response)
{
    const QSignalBlocker blocker(ui->renderDeviceCombo);
    ui->renderDeviceCombo->clear();
    ui->renderDeviceCombo->addItem(tr("自动"), QStringLiteral("auto"));
    for (const auto& value : response.value(QStringLiteral("devices")).toArray()) {
        const auto device = value.toObject();
        const auto id = device.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id == QStringLiteral("auto") || !ValidRenderDevice(id.toStdString()) ||
            ui->renderDeviceCombo->findData(id) >= 0) continue;
        ui->renderDeviceCombo->addItem(QStringLiteral("%1 (%2)").arg(device.value(QStringLiteral("name")).toString(), id), id);
    }
    auto saved = response.value(QStringLiteral("device")).toString();
    if (saved.isEmpty()) saved = QStringLiteral("auto");
    if (ui->renderDeviceCombo->findData(saved) < 0) ui->renderDeviceCombo->addItem(tr("%1（不可用）").arg(saved), saved);
    ui->renderDeviceCombo->setCurrentIndex(ui->renderDeviceCombo->findData(saved));
    const auto active = response.value(QStringLiteral("active_device")).toString();
    ui->renderDeviceStatus->setText(response.value(QStringLiteral("restart_required")).toBool()
        ? tr("已保存，需点击 Respawn 重启生效。当前启动选择：%1").arg(active == QStringLiteral("auto") ? tr("自动") : active)
        : tr("已保存；更改渲染设备后需重启生效。"));
}

void MainWindow::requestTransform()
{
    if (_transformQueryPending || _transformEditStarting || _transformPreviewInFlight || _transformPreviewPending) return;
    _transformQueryPending = true;
    _client.sendCommand(QStringLiteral("get_transform_settings"));
}

void MainWindow::requestHoverOpacity()
{
    if (_hoverQueryPending || _hoverSetPending || ui->hoverOpacitySlider->isSliderDown() || ui->idleOpacitySlider->isSliderDown()) return;
    _hoverQueryPending = true;
    _client.sendCommand(QStringLiteral("get_hover_opacity"));
}

void MainWindow::setHoverOpacity(const QJsonObject& parameters)
{
    if (!_client.isConnected() || !_hoverSupported || _hoverSetPending) return;
    _hoverSetPending = true;
    ui->hoverOpacityGroup->setEnabled(false);
    setStatusMessage(tr("正在设置并保存…"));
    _client.sendCommand(QStringLiteral("set_hover_opacity"), parameters);
}

void MainWindow::showHoverOpacity(const QJsonObject& response)
{
    const QSignalBlocker blockEnabled(ui->hoverOpacityEnabledCheck);
    const QSignalBlocker blockOpacity(ui->hoverOpacitySlider);
    const QSignalBlocker blockIdleOpacity(ui->idleOpacitySlider);
    ui->hoverOpacityEnabledCheck->setChecked(response.value(QStringLiteral("enabled")).toBool());
    const int value = qRound(response.value(QStringLiteral("opacity")).toDouble() * 100);
    ui->hoverOpacitySlider->setValue(value);
    ui->hoverOpacityValueLabel->setText(QStringLiteral("%1%").arg(value));
    const int idleValue = qRound(response.value(QStringLiteral("idle_opacity")).toDouble(1) * 100);
    ui->idleOpacitySlider->setValue(idleValue);
    ui->idleOpacityValueLabel->setText(QStringLiteral("%1%").arg(idleValue));
}

void MainWindow::requestMouseLook()
{
    if (_mouseLookQueryPending || _mouseLookSetPending || ui->mouseLookRadiusSlider->isSliderDown() ||
        ui->mouseLookRadiusSpin->hasFocus()) return;
    _mouseLookQueryPending = true;
    _client.sendCommand(QStringLiteral("get_mouse_look"));
}

void MainWindow::setMouseLook(double radius)
{
    if (!_client.isConnected() || !_mouseLookSupported || _mouseLookSetPending) return;
    _mouseLookSetPending = true;
    ui->mouseLookGroup->setEnabled(false);
    setStatusMessage(tr("正在设置并保存…"));
    _client.sendCommand(QStringLiteral("set_mouse_look"), {{QStringLiteral("radius"), radius}});
}

void MainWindow::showMouseLook(double radius)
{
    const QSignalBlocker blockSlider(ui->mouseLookRadiusSlider);
    const QSignalBlocker blockSpin(ui->mouseLookRadiusSpin);
    ui->mouseLookRadiusSpin->setMaximum(std::max(10.0, radius));
    ui->mouseLookRadiusSpin->setValue(radius);
    ui->mouseLookRadiusSlider->setValue(qRound(std::min(radius, 2.0) * 100));
}

void MainWindow::sendTransformSave(const QString& command, QJsonObject parameters)
{
    if (!_transformSaveRequest.isEmpty()) return;
    _transformSaveRequest = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setStatusMessage(tr("正在保存变换设置…"));
    ui->outputCombo->setEnabled(false);
    ui->transformGroup->setEnabled(false); ui->positionEdit->setEnabled(false);
    ui->cancelTransformButton->setEnabled(false); ui->relocateCropButton->setEnabled(false);
    ui->retryTransformButton->setEnabled(false); ui->xformResetButton->setEnabled(false);
    parameters.insert(QStringLiteral("request_id"), _transformSaveRequest);
    _client.sendCommand(command, parameters);
}

QJsonObject MainWindow::transformParameters() const
{
    return {{QStringLiteral("x"), ui->positionXSpin->value()}, {QStringLiteral("y"), ui->positionYSpin->value()},
        {QStringLiteral("zoom"), ui->zoomSpin->value()}, {QStringLiteral("cropped"), ui->cropEnabledCheck->isChecked()},
        {QStringLiteral("crop_x"), ui->cropBox_x->value()}, {QStringLiteral("crop_y"), ui->cropBox_y->value()},
        {QStringLiteral("crop_width"), ui->cropBox_w->value()}, {QStringLiteral("crop_height"), ui->cropBox_h->value()}};
}

void MainWindow::queueTransformPreview()
{
    if (_transformUpdating || !_client.isConnected() || !_transformSupported) return;
    _transformPreviewPending = true;
    ui->outputCombo->setEnabled(false);
    ui->transformButton->setEnabled(false);
    if (_transformEditStarting || _transformPreviewInFlight) return;
    if (!_transformOwned) {
        _transformEditStarting = true;
        _client.sendCommand(QStringLiteral("begin_transform_edit"),
            {{QStringLiteral("expected_revision"), _lastTransformResponse.value(QStringLiteral("revision"))}});
        return;
    }
    _transformPreviewPending = false;
    _transformPreviewInFlight = true;
    _client.sendCommand(QStringLiteral("update_transform_edit"), transformParameters());
}

void MainWindow::showTransform(const QJsonObject& response)
{
    _lastTransformResponse = response;
    _transformUpdating = true;
    {
        const QSignalBlocker blocker(ui->outputCombo);
        QStringList names;
        QString currentName;
        for (const auto& value : response.value(QStringLiteral("outputs")).toArray()) {
            const auto output = value.toObject();
            const auto name = output.value(QStringLiteral("name")).toString();
            if (name.isEmpty()) continue;
            names.append(name);
            if (output.value(QStringLiteral("current")).toBool()) currentName = name;
        }
        QStringList previousNames;
        for (int i = 0; i < ui->outputCombo->count(); ++i) previousNames.append(ui->outputCombo->itemData(i).toString());
        if (names != previousNames) {
            ui->outputCombo->clear();
            for (const auto& name : names) ui->outputCombo->addItem(name, name);
        }
        int current = -1;
        if (!currentName.isEmpty()) current = ui->outputCombo->findData(currentName);
        ui->outputCombo->setCurrentIndex(current);
        ui->outputCombo->setEnabled(!_outputSwitchPending && _transformSaveRequest.isEmpty() &&
            !_transformEditStarting && !_transformPreviewPending &&
            !response.value(QStringLiteral("editing")).toBool() && ui->outputCombo->count() > 0);
    }
    ui->positionXSpin->setValue(response.value(QStringLiteral("x")).toDouble());
    ui->positionYSpin->setValue(response.value(QStringLiteral("y")).toDouble());
    ui->zoomSpin->setValue(response.value(QStringLiteral("zoom")).toDouble());
    _surfaceWidth = response.value(QStringLiteral("surface_width")).toInt(1);
    _surfaceHeight = response.value(QStringLiteral("surface_height")).toInt(1);
    const bool cropped = response.value(QStringLiteral("cropped")).toBool();
    ui->cropEnabledCheck->setChecked(cropped);
    ui->cropBox_x->setValue(response.value(QStringLiteral("crop_x")).toInt());
    ui->cropBox_y->setValue(response.value(QStringLiteral("crop_y")).toInt());
    ui->cropBox_w->setValue(cropped ? response.value(QStringLiteral("crop_width")).toInt(1) : _surfaceWidth);
    ui->cropBox_h->setValue(cropped ? response.value(QStringLiteral("crop_height")).toInt(1) : _surfaceHeight);
    for (auto* spin : {ui->cropBox_x, ui->cropBox_y, ui->cropBox_w, ui->cropBox_h}) spin->setEnabled(cropped);
    const bool busy = _outputSwitchPending || (response.value(QStringLiteral("editing")).toBool() && !_transformOwned);
    ui->transformGroup->setEnabled(!busy); ui->positionEdit->setEnabled(!busy);
    ui->transformButton->setEnabled(_transformOwned);
    ui->cancelTransformButton->setEnabled(_transformOwned);
    ui->relocateCropButton->setEnabled(_transformOwned && cropped);
    ui->retryTransformButton->setEnabled(response.value(QStringLiteral("unsaved")).toBool() && !response.value(QStringLiteral("editing")).toBool());
    ui->xformResetButton->setEnabled(!busy);
    QString status = busy ? (response.value(QStringLiteral("local_screen_editing")).toBool()
        ? tr("正在模型窗口编辑，双击裁剪框线以保存并隐藏边框") : tr("另一个控制窗口正在编辑"))
        : _transformOwned ? tr("编辑预览：应用并保存或取消，也可双击裁剪框线保存。顶部中央手柄同步移动模型和裁剪框，边角调整大小。")
        : response.value(QStringLiteral("unsaved")).toBool() ? tr("有未保存修改") : tr("与配置同步");
    if (_transformConflict) status += tr("；检测到外部配置冲突");
    const QString warning = response.value(QStringLiteral("warning")).toString();
    if (!warning.isEmpty()) status += QStringLiteral("\n") + warning;
    status += QStringLiteral("\n") + response.value(QStringLiteral("config_path")).toString();
    ui->transformStatusLabel->setText(status);
    _transformUpdating = false;
}
