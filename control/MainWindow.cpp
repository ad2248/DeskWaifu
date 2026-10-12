#include "MainWindow.hpp"
#include "ui_MainWindow.h"
#include "HotkeyPanel.hpp"
#include <QTimer>
#include <QUuid>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QVariantMap>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
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
        if (_client.isConnected() && isActiveWindow()) requestHotkeys();
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
            if (!connected)
            {
                _transformOwned = _transformEditStarting = _transformQueryPending = false;
                _transformPreviewInFlight = _transformPreviewPending = _transformSupported = false;
                ui->transformStatusLabel->setText(tr("连接断开：未确认的编辑已取消，请重新查询保存结果。"));
                _hotkeyModelToken.clear();
                _lastHotkeyResponse = {};
                _hotkeyQueryPending = false;
            }
            statusBar()->showMessage(message);
            ui->transformGroup->setEnabled(connected && _transformSupported);
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
        [this](const QString& message) { ui->responseLog->appendPlainText(message); });
    connect(&_client, &DeskWaifuClient::commandOutcomeUnknown, this,
        [this](const QString& command) {
            const QString message = tr("Connection lost: outcome of '%1' is unknown; command was not retried.")
                .arg(command);
            ui->responseLog->appendPlainText(message);
            statusBar()->showMessage(message, 8000);
        });

    _client.connectToServer();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::bindControls()
{
    ui->horizontalLayout->setColumnStretch(1, 1);
    ui->horizontalLayout->setColumnStretch(3, 1);
    ui->horizontalLayout_2->setColumnStretch(1, 1);
    ui->horizontalLayout_2->setColumnStretch(3, 1);
    connect(ui->refreshButton, &QPushButton::clicked, this, &MainWindow::refreshAll);
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
    ui->xformResetButton->setEnabled(false); ui->cancelTransformButton->setEnabled(false);
    ui->relocateCropButton->setEnabled(false); ui->retryTransformButton->setEnabled(false);
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
}

void MainWindow::handleResponse(const QString& command, const QJsonObject& response)
{
    if (command == QStringLiteral("get_transform_save_result")) {
        const QString message = !response.value(QStringLiteral("known")).toBool() && response.value(QStringLiteral("ok")).toBool()
            ? tr("无法确认上次保存结果（后端可能已重启），请核对当前配置。")
            : response.value(QStringLiteral("ok")).toBool() ? tr("上次保存已成功。") : response.value(QStringLiteral("error")).toString();
        ui->responseLog->appendPlainText(message);
        statusBar()->showMessage(message, 10000);
        _transformSaveRequest.clear();
        requestTransform();
        return;
    }
    if (command == QStringLiteral("apply_transform_edit") || command == QStringLiteral("reset_transform_settings") || command == QStringLiteral("retry_transform_save"))
        _transformSaveRequest.clear();
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
            ui->transformStatusLabel->setText(response.value(QStringLiteral("error")).toString());
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
        statusBar()->showMessage(response.value(QStringLiteral("error")).toString(), 5000);
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

void MainWindow::requestTransform()
{
    if (_transformQueryPending || _transformEditStarting || _transformPreviewInFlight || _transformPreviewPending) return;
    _transformQueryPending = true;
    _client.sendCommand(QStringLiteral("get_transform_settings"));
}

void MainWindow::sendTransformSave(const QString& command, QJsonObject parameters)
{
    if (!_transformSaveRequest.isEmpty()) return;
    _transformSaveRequest = QUuid::createUuid().toString(QUuid::WithoutBraces);
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
    const bool busy = response.value(QStringLiteral("editing")).toBool() && !_transformOwned;
    ui->transformGroup->setEnabled(!busy); ui->positionEdit->setEnabled(!busy);
    ui->transformButton->setEnabled(_transformOwned);
    ui->cancelTransformButton->setEnabled(_transformOwned);
    ui->relocateCropButton->setEnabled(_transformOwned && cropped);
    ui->retryTransformButton->setEnabled(response.value(QStringLiteral("unsaved")).toBool() && !response.value(QStringLiteral("editing")).toBool());
    ui->xformResetButton->setEnabled(!busy);
    QString status = busy ? tr("另一个控制窗口正在编辑") : _transformOwned ? tr("编辑预览：应用并保存或取消。顶部中央手柄同步移动模型和裁剪框，边角调整大小。")
        : response.value(QStringLiteral("unsaved")).toBool() ? tr("有未保存修改") : tr("与配置同步");
    if (_transformConflict) status += tr("；检测到外部配置冲突");
    const QString warning = response.value(QStringLiteral("warning")).toString();
    if (!warning.isEmpty()) status += QStringLiteral("\n") + warning;
    status += QStringLiteral("\n") + response.value(QStringLiteral("config_path")).toString();
    ui->transformStatusLabel->setText(status);
    _transformUpdating = false;
}
