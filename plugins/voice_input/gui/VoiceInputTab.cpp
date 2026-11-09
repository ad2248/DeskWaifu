#include "VoiceInputTab.hpp"
#include "plugin/GuiHostServices.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
const QString settingsCommand = QStringLiteral("get_voice_input_settings");
const QString setSettingsCommand = QStringLiteral("set_voice_input_settings");
const QString statusCommand = QStringLiteral("get_voice_input_status");
const QString devicesCommand = QStringLiteral("list_voice_input_devices");
const QString resultCommand = QStringLiteral("get_voice_input_result");
const QStringList mutations{setSettingsCommand, QStringLiteral("start_voice_input"),
    QStringLiteral("toggle_voice_input"), QStringLiteral("stop_voice_input"),
    QStringLiteral("cancel_voice_input"), QStringLiteral("retry_voice_input_copy")};
}

VoiceInputTab::VoiceInputTab(GuiHostServices& services) : _client(services)
{
    setObjectName(QStringLiteral("voiceInputTab"));
    setWidgetResizable(true);
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    auto* form = new QFormLayout;
    _endpoint = new QLineEdit(content);
    _endpoint->setObjectName(QStringLiteral("voiceInputEndpoint"));
    _endpoint->setPlaceholderText(QStringLiteral("https://…/compatible-mode/v1"));
    _apiKey = new QLineEdit(content);
    _apiKey->setObjectName(QStringLiteral("voiceInputApiKey"));
    _apiKey->setEchoMode(QLineEdit::Password);
    _apiKey->setPlaceholderText(tr("留空保留已保存的密钥"));
    _model = new QLineEdit(content);
    _model->setObjectName(QStringLiteral("voiceInputModel"));
    _device = new QComboBox(content);
    _device->setObjectName(QStringLiteral("voiceInputDevice"));
    _device->addItem(tr("系统默认输入设备"), QString());
    _maxSeconds = new QSpinBox(content);
    _maxSeconds->setObjectName(QStringLiteral("voiceInputMaxRecordingSeconds"));
    _maxSeconds->setRange(1, 300);
    _maxSeconds->setSuffix(tr(" 秒"));
    form->addRow(tr("ASR Endpoint"), _endpoint);
    form->addRow(tr("API Key"), _apiKey);
    form->addRow(tr("模型"), _model);
    form->addRow(tr("录音设备"), _device);
    form->addRow(tr("最长录音时长"), _maxSeconds);
    layout->addLayout(form);
    auto* endpointHint = new QLabel(tr("兼容模式可填写 /v1 基础地址；qwen-audio-3.0-asr-flash 使用同一服务的原生 ASR 路径。音频将上传至所配置的服务。"), content);
    endpointHint->setWordWrap(true);
    layout->addWidget(endpointHint);
    auto* settingsButtons = new QHBoxLayout;
    _save = new QPushButton(tr("保存设置"), content);
    _save->setObjectName(QStringLiteral("saveVoiceInputSettings"));
    _clearKey = new QPushButton(tr("清除已保存的 API Key"), content);
    _clearKey->setObjectName(QStringLiteral("clearVoiceInputKey"));
    settingsButtons->addWidget(_save);
    settingsButtons->addWidget(_clearKey);
    layout->addLayout(settingsButtons);
    _settingsStatus = new QLabel(tr("未连接"), content);
    _settingsStatus->setObjectName(QStringLiteral("voiceInputSettingsStatus"));
    _settingsStatus->setWordWrap(true);
    layout->addWidget(_settingsStatus);
    auto* recordingButtons = new QHBoxLayout;
    _start = new QPushButton(tr("开始录音"), content);
    _start->setObjectName(QStringLiteral("startVoiceInput"));
    _stop = new QPushButton(tr("停止并识别"), content);
    _stop->setObjectName(QStringLiteral("stopVoiceInput"));
    _cancel = new QPushButton(tr("取消本次录音"), content);
    _cancel->setObjectName(QStringLiteral("cancelVoiceInput"));
    recordingButtons->addWidget(_start);
    recordingButtons->addWidget(_stop);
    recordingButtons->addWidget(_cancel);
    layout->addLayout(recordingButtons);
    _status = new QLabel(tr("未连接"), content);
    _status->setObjectName(QStringLiteral("voiceInputStatus"));
    _status->setWordWrap(true);
    layout->addWidget(_status);
    _result = new QPlainTextEdit(content);
    _result->setObjectName(QStringLiteral("voiceInputResult"));
    _result->setReadOnly(true);
    _result->setPlaceholderText(tr("最近一次识别文本；不会自动粘贴或发送"));
    layout->addWidget(_result);
    _copyStatus = new QLabel(content);
    _copyStatus->setObjectName(QStringLiteral("voiceInputCopyStatus"));
    layout->addWidget(_copyStatus);
    _retryCopy = new QPushButton(tr("重新复制最近文本"), content);
    _retryCopy->setObjectName(QStringLiteral("retryVoiceInputCopy"));
    layout->addWidget(_retryCopy);
    setWidget(content);

    _pollTimer.setSingleShot(true);
    _pollTimer.setInterval(500);
    connect(&_pollTimer, &QTimer::timeout, this, &VoiceInputTab::poll);
    connect(_save, &QPushButton::clicked, this, [this] { save(false); });
    connect(_clearKey, &QPushButton::clicked, this, [this] { save(true); });
    connect(_start, &QPushButton::clicked, this, [this] { mutate(QStringLiteral("start_voice_input")); });
    connect(_stop, &QPushButton::clicked, this, [this] {
        mutate(QStringLiteral("stop_voice_input"), {{QStringLiteral("session_id"), _sessionId}});
    });
    connect(_cancel, &QPushButton::clicked, this, [this] {
        mutate(QStringLiteral("cancel_voice_input"), {{QStringLiteral("session_id"), _sessionId}});
    });
    connect(_retryCopy, &QPushButton::clicked, this, [this] {
        mutate(QStringLiteral("retry_voice_input_copy"), {{QStringLiteral("session_id"), _resultSession}});
    });
    connect(&_client, &GuiHostServices::connectionChanged, this, [this](bool connected, const QString&) {
        if (!_started) return;
        if (connected) refresh();
        else {
            _pollTimer.stop();
            cancelUnsent();
            _mutation.clear();
            _settingsPending = _statusPending = _devicesPending = false;
            _statusKnown = _discardStatus = false;
            _resultPending.clear();
            _resultRequested.clear();
            _apiKey->clear();
            _status->setText(tr("未连接；未确认的操作不会自动重试"));
            _settingsStatus->setText(tr("未连接；重连后查询保存结果"));
        }
        updateControls();
    });
    connect(&_client, &GuiHostServices::activationChanged, this, [this](bool active) {
        if (!active) {
            _pollTimer.stop();
            _statusKnown = false;
        } else poll();
        updateControls();
    });
    connect(&_client, &GuiHostServices::responseReceived, this, &VoiceInputTab::handleResponse);
    connect(&_client, &GuiHostServices::commandOutcomeUnknown, this, [this](const QString& command) {
        if (!_started || command != _mutation) return;
        _mutation.clear();
        _statusKnown = false;
        _apiKey->clear();
        _status->setText(tr("操作结果未知；不会自动重试"));
        updateControls();
    });
    updateControls();
}

VoiceInputTab::~VoiceInputTab() { stop(); }

void VoiceInputTab::start()
{
    if (_started) return;
    _started = true;
    refresh();
    updateControls();
}

void VoiceInputTab::cancelUnsent()
{
    for (const auto& command : mutations) _client.cancelQueuedCommand(command);
    for (const auto& command : {settingsCommand, statusCommand, devicesCommand, resultCommand})
        _client.cancelQueuedCommand(command);
}

void VoiceInputTab::stop()
{
    if (!_started) return;
    _started = false;
    _pollTimer.stop();
    cancelUnsent();
    _apiKey->clear();
    _mutation.clear();
    _settingsPending = _statusPending = _devicesPending = false;
    _statusKnown = _discardStatus = false;
    _resultPending.clear();
    _resultRequested.clear();
    updateControls();
}

void VoiceInputTab::showEvent(QShowEvent* event)
{
    QScrollArea::showEvent(event);
    _pollingVisible = true;
    poll();
    updateControls();
}

void VoiceInputTab::hideEvent(QHideEvent* event)
{
    QScrollArea::hideEvent(event);
    _pollingVisible = false;
    _statusKnown = false;
    _pollTimer.stop();
    updateControls();
}

bool VoiceInputTab::canPoll() const
{
    return _started && _pollingVisible && _client.isActive() && _client.isConnected();
}

void VoiceInputTab::refresh()
{
    if (!_started || !_client.isConnected()) return;
    if (!_settingsPending) {
        _settingsPending = true;
        _client.sendCommand(settingsCommand);
    }
    if (!_devicesPending) {
        _devicesPending = true;
        _client.sendCommand(devicesCommand);
    }
    poll();
}

void VoiceInputTab::poll()
{
    if (!canPoll() || _statusPending) return;
    if (!_mutation.isEmpty() && _mutation != setSettingsCommand && _mutation != QStringLiteral("retry_voice_input_copy")) {
        _pollTimer.start();
        return;
    }
    _statusPending = true;
    _client.sendCommand(statusCommand);
}

void VoiceInputTab::updateControls()
{
    const bool ready = _started && _client.isActive() && _client.isConnected() && _mutation.isEmpty();
    _save->setEnabled(ready && !_settingsPending);
    _clearKey->setEnabled(ready && !_settingsPending && _configured);
    _start->setEnabled(ready && _statusKnown && _state == QStringLiteral("idle") && _phase == QStringLiteral("idle") && _configured);
    const bool recording = _state == QStringLiteral("recording") && _phase == QStringLiteral("recording") && !_sessionId.isEmpty();
    _stop->setEnabled(ready && _statusKnown && recording);
    _cancel->setEnabled(ready && _statusKnown && !_sessionId.isEmpty() && _state != QStringLiteral("idle"));
    _retryCopy->setEnabled(ready && !_resultSession.isEmpty() && !_result->toPlainText().isEmpty());
}

void VoiceInputTab::save(bool clearKey)
{
    if (!_started || !_client.isActive() || !_client.isConnected() || !_mutation.isEmpty()) return;
    QJsonObject parameters{{QStringLiteral("endpoint"), _endpoint->text().trimmed()},
        {QStringLiteral("model"), _model->text().trimmed()},
        {QStringLiteral("device_id"), _device->currentData().toString()},
        {QStringLiteral("max_recording_seconds"), _maxSeconds->value()}};
    const auto key = _apiKey->text().trimmed();
    if (clearKey || !key.isEmpty()) parameters.insert(QStringLiteral("api_key"), clearKey ? QString() : key);
    _apiKey->clear();
    _settingsStatus->setText(tr("正在保存…"));
    mutate(setSettingsCommand, parameters);
}

void VoiceInputTab::mutate(const QString& command, const QJsonObject& parameters)
{
    if (!_started || !_client.isActive() || !_client.isConnected() || !_mutation.isEmpty()) return;
    _mutation = command;
    if (command != setSettingsCommand && command != QStringLiteral("retry_voice_input_copy")) {
        _discardStatus = _statusPending;
        _statusKnown = false;
    }
    updateControls();
    _client.sendCommand(command, parameters);
}

void VoiceInputTab::handleResponse(const QString& command, const QJsonObject& response)
{
    if (!_started || !_client.isConnected()) return;
    const bool ok = response.value(QStringLiteral("ok")).toBool();
    if (command == settingsCommand || command == setSettingsCommand) {
        if (command == settingsCommand) {
            if (_mutation == setSettingsCommand || !_settingsPending) return;
            _settingsPending = false;
        } else {
            if (_mutation != command) return;
            _mutation.clear();
            _settingsPending = false;
            _apiKey->clear();
        }
        if (ok && response.value(QStringLiteral("configured")).isBool()) {
            _configured = response.value(QStringLiteral("configured")).toBool();
            _endpoint->setText(response.value(QStringLiteral("endpoint")).toString());
            _model->setText(response.value(QStringLiteral("model")).toString());
            _maxSeconds->setValue(response.value(QStringLiteral("max_recording_seconds")).toInt(60));
            const auto deviceId = response.value(QStringLiteral("device_id")).toString();
            int index = _device->findData(deviceId);
            if (index < 0) {
                _device->addItem(deviceId, deviceId);
                index = _device->count() - 1;
            }
            _device->setCurrentIndex(index);
            _settingsStatus->setText(_configured ? tr("API Key 已保存；为安全起见不回显") : tr("未配置 API Key"));
        } else _settingsStatus->setText(tr("语音输入设置失败，请检查配置或后端版本"));
        // Never display raw settings response/error payloads: they may contain credentials.
    } else if (command == devicesCommand) {
        if (!_devicesPending) return;
        _devicesPending = false;
        if (ok) {
            const auto selected = _device->currentData().toString();
            _device->clear();
            _device->addItem(tr("系统默认输入设备"), QString());
            for (const auto& entry : response.value(QStringLiteral("devices")).toArray()) {
                const auto device = entry.toObject();
                const auto id = device.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) continue;
                auto name = device.value(QStringLiteral("name")).toString();
                if (device.value(QStringLiteral("is_default")).toBool()) name += tr("（默认）");
                _device->addItem(name, id);
            }
            int index = _device->findData(selected);
            if (index < 0) {
                _device->addItem(selected, selected);
                index = _device->count() - 1;
            }
            _device->setCurrentIndex(index);
        }
    } else if (command == statusCommand) {
        if (!_statusPending) return;
        _statusPending = false;
        if (_discardStatus) {
            // A read sent before a recording mutation cannot confirm its outcome.
            _discardStatus = false;
            poll();
            updateControls();
            return;
        }
        _statusKnown = ok && canPoll();
        if (ok) {
            _state = response.value(QStringLiteral("state")).toString();
            _phase = response.value(QStringLiteral("phase")).toString();
            _sessionId = response.value(QStringLiteral("session_id")).toString();
            _status->setText(tr("状态：%1 / %2；时长：%3 秒；结果：%4")
                .arg(_state, _phase).arg(response.value(QStringLiteral("elapsed_ms")).toDouble() / 1000.0, 0, 'f', 1)
                .arg(response.value(QStringLiteral("last_outcome")).toString()));
            const auto errorCode = response.value(QStringLiteral("error_code")).toString();
            if (!errorCode.isEmpty()) _status->setText(_status->text() + tr("；错误：%1").arg(errorCode));
            _copyStatus->setText(tr("剪贴板：%1").arg(response.value(QStringLiteral("copy_state")).toString()));
            if (canPoll() && _state == QStringLiteral("idle") && !_sessionId.isEmpty() && _sessionId != QStringLiteral("0")
                && _sessionId != _resultRequested && _resultPending.isEmpty()) {
                _resultRequested = _resultPending = _sessionId;
                _client.sendCommand(resultCommand, {{QStringLiteral("session_id"), _sessionId}});
            }
        } else _status->setText(tr("状态查询失败"));
        if (canPoll()) _pollTimer.start();
    } else if (command == resultCommand) {
        if (_resultPending.isEmpty()) return;
        // Only one result request is in flight. Even stale_session errors may carry
        // a different session ID, so always release it before checking the payload.
        const auto requestedSession = _resultPending;
        _resultPending.clear();
        if (ok && requestedSession == _sessionId && _state == QStringLiteral("idle")
            && response.value(QStringLiteral("session_id")).toString() == requestedSession) {
            _resultSession = requestedSession;
            _result->setPlainText(response.value(QStringLiteral("text")).toString());
            _copyStatus->setText(tr("剪贴板：%1").arg(response.value(QStringLiteral("copy_state")).toString()));
        } else if (_resultRequested == requestedSession) {
            // A transient failure (for example, copying in another client) must
            // not suppress the next idle poll's result read for this session.
            _resultRequested.clear();
        }
    } else if (command == _mutation) {
        _mutation.clear();
        if (command == QStringLiteral("retry_voice_input_copy"))
            _copyStatus->setText(ok ? tr("剪贴板：%1").arg(response.value(QStringLiteral("copy_state")).toString())
                : tr("复制失败：%1").arg(response.value(QStringLiteral("error_code")).toString()));
        else {
            _statusKnown = false;
            if (!ok) _status->setText(tr("录音操作失败，请查询当前状态后重试"));
            poll();
        }
    }
    updateControls();
}
