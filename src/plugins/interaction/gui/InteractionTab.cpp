#include "InteractionTab.hpp"
#include "ui_InteractionTab.h"
#include "plugin/GuiHostServices.hpp"
#include "HotkeyPanel.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <cmath>

InteractionTab::InteractionTab(GuiHostServices& services) : ui(new Ui::InteractionTab), _client(services)
{
    ui->setupUi(this);
    _hotkeys = new HotkeyPanel(ui->hotkeyGroup);
    _hotkeys->setMinimumHeight(200);
    ui->hotkeyLayout->addWidget(_hotkeys);
    _hotkeys->setConnected(_client.isConnected());
    connect(_hotkeys, &HotkeyPanel::triggered, this, [this](const QString& id) {
        if (_client.isConnected() && !_hotkeyModelToken.isEmpty())
            _client.sendCommand(QStringLiteral("trigger_hotkey"), {
                {QStringLiteral("id"), id}, {QStringLiteral("model_token"), _hotkeyModelToken}});
    });
    auto* timer = new QTimer(this);
    timer->setInterval(1500);
    connect(timer, &QTimer::timeout, this, [this] {
        if (_client.isConnected() && window()->isActiveWindow()) refreshAll();
    });
    timer->start();
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
            {QStringLiteral("x"), ui->lookXSpin->value()}, {QStringLiteral("y"), ui->lookYSpin->value()}});
    });
    connect(ui->resetLookButton, &QPushButton::clicked, this, [this] {
        _client.sendCommand(QStringLiteral("set_look"), {{QStringLiteral("reset"), true}});
    });
    connect(&_client, &GuiHostServices::connectionChanged, this, [this](bool connected, const QString&) {
        _hotkeys->setConnected(connected);
        if (!connected) {
            if (_hoverSetPending || _mouseLookSetPending)
                setStatusMessage(tr("连接断开：设置结果未确认，重新连接后将查询实际保存结果。"));
            _hotkeyModelToken.clear();
            _lastHotkeyResponse = {};
            _hotkeyQueryPending = false;
            _hoverQueryPending = _hoverSetPending = _hoverSupported = false;
            _mouseLookQueryPending = _mouseLookSetPending = _mouseLookSupported = false;
        }
        ui->hoverOpacityGroup->setEnabled(false);
        ui->mouseLookGroup->setEnabled(false);
        if (connected) refreshAll();
    });
    connect(&_client, &GuiHostServices::responseReceived, this, &InteractionTab::handleResponse);
}

void InteractionTab::start() { if (_client.isConnected()) refreshAll(); }

InteractionTab::~InteractionTab() { delete ui; }
void InteractionTab::setStatusMessage(const QString& message, int timeout) { _client.setStatusMessage(message, timeout); }
void InteractionTab::logResponse(const QString& command, const QJsonObject& response)
{
    _client.publishLog(QStringLiteral("%1  %2").arg(command, QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))));
}
void InteractionTab::refreshAll() { requestHotkeys(); requestHoverOpacity(); requestMouseLook(); }

void InteractionTab::handleResponse(const QString& command, const QJsonObject& response)
{
    if (command == QStringLiteral("get_mouse_look") || command == QStringLiteral("set_mouse_look")) {
        const bool mutation = command == QStringLiteral("set_mouse_look");
        if (mutation) _mouseLookSetPending = false;
        else _mouseLookQueryPending = false;
        const auto value = response.value(QStringLiteral("radius"));
        const double radius = value.toDouble(-1);
        const bool valid = value.isDouble() && std::isfinite(radius) && radius >= 0;
        const bool ok = response.value(QStringLiteral("ok")).toBool() && valid;
        if (!mutation && (_mouseLookSetPending || (ok && _mouseLookSupported &&
            (ui->mouseLookRadiusSlider->isSliderDown() || ui->mouseLookRadiusSpin->hasFocus())))) return;
        if (valid) { _mouseLookSupported = true; _lastMouseLookRadius = radius; }
        else if (!mutation) _mouseLookSupported = false;
        showMouseLook(_lastMouseLookRadius);
        ui->mouseLookGroup->setEnabled(_client.isConnected() && _mouseLookSupported);
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
        // Queries issued before a drag or queued mutation must not overwrite user input.
        if (!mutation && (_hoverSetPending || ui->hoverOpacitySlider->isSliderDown() || ui->idleOpacitySlider->isSliderDown())) return;
        const double opacity = response.value(QStringLiteral("opacity")).toDouble(-1);
        const auto idleOpacity = response.value(QStringLiteral("idle_opacity"));
        const bool valid = response.value(QStringLiteral("enabled")).isBool()
            && response.value(QStringLiteral("opacity")).isDouble()
            && std::isfinite(opacity) && opacity >= 0 && opacity <= 1
            && (idleOpacity.isUndefined() || (idleOpacity.isDouble() && std::isfinite(idleOpacity.toDouble())
                && idleOpacity.toDouble() >= 0 && idleOpacity.toDouble() <= 1));
        if (valid) { _hoverSupported = true; _lastHoverResponse = response; showHoverOpacity(response); }
        else {
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
    if (command == QStringLiteral("set_model") || command == QStringLiteral("next_model") || command == QStringLiteral("prev_model")) {
        if (response.value(QStringLiteral("ok")).toBool()) refreshAll();
        return;
    }
    if (command == QStringLiteral("set_look")) {
        logResponse(command, response);
        if (!response.value(QStringLiteral("ok")).toBool()) setStatusMessage(response.value(QStringLiteral("error")).toString(), 5000);
        return;
    }
    if (command != QStringLiteral("get_hotkeys") && command != QStringLiteral("trigger_hotkey")) return;
    if (command != QStringLiteral("get_hotkeys") || response != _lastHotkeyResponse) logResponse(command, response);
    if (command == QStringLiteral("get_hotkeys")) _hotkeyQueryPending = false;
    if (response.value(QStringLiteral("ok")).toBool()) {
        if (response != _lastHotkeyResponse) {
            if (_hotkeyModelToken != response.value(QStringLiteral("model_token")).toString()) _hotkeys->resetInput();
            _hotkeys->setBindings(response.value(QStringLiteral("hotkeys")).toArray(), response.value(QStringLiteral("warning")).toString());
            _hotkeyModelToken = response.value(QStringLiteral("model_token")).toString();
            _lastHotkeyResponse = response;
        }
    } else {
        if (command == QStringLiteral("get_hotkeys")) {
            _hotkeyModelToken.clear();
            _lastHotkeyResponse = {};
            _hotkeys->setBindings({}, response.value(QStringLiteral("error")).toString());
        } else requestHotkeys();
        setStatusMessage(response.value(QStringLiteral("error")).toString(), 5000);
    }
}

void InteractionTab::requestHotkeys()
{
    if (_hotkeyQueryPending) return;
    _hotkeyQueryPending = true;
    _client.sendCommand(QStringLiteral("get_hotkeys"));
}
void InteractionTab::requestHoverOpacity()
{
    if (_hoverQueryPending || _hoverSetPending || ui->hoverOpacitySlider->isSliderDown() || ui->idleOpacitySlider->isSliderDown()) return;
    _hoverQueryPending = true;
    _client.sendCommand(QStringLiteral("get_hover_opacity"));
}
void InteractionTab::setHoverOpacity(const QJsonObject& parameters)
{
    if (!_client.isConnected() || !_hoverSupported || _hoverSetPending) return;
    _hoverSetPending = true;
    ui->hoverOpacityGroup->setEnabled(false);
    setStatusMessage(tr("正在设置并保存…"));
    _client.sendCommand(QStringLiteral("set_hover_opacity"), parameters);
}
void InteractionTab::showHoverOpacity(const QJsonObject& response)
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
void InteractionTab::requestMouseLook()
{
    if (_mouseLookQueryPending || _mouseLookSetPending || ui->mouseLookRadiusSlider->isSliderDown() || ui->mouseLookRadiusSpin->hasFocus()) return;
    _mouseLookQueryPending = true;
    _client.sendCommand(QStringLiteral("get_mouse_look"));
}
void InteractionTab::setMouseLook(double radius)
{
    if (!_client.isConnected() || !_mouseLookSupported || _mouseLookSetPending) return;
    _mouseLookSetPending = true;
    ui->mouseLookGroup->setEnabled(false);
    setStatusMessage(tr("正在设置并保存…"));
    _client.sendCommand(QStringLiteral("set_mouse_look"), {{QStringLiteral("radius"), radius}});
}
void InteractionTab::showMouseLook(double radius)
{
    const QSignalBlocker blockSlider(ui->mouseLookRadiusSlider);
    const QSignalBlocker blockSpin(ui->mouseLookRadiusSpin);
    ui->mouseLookRadiusSpin->setMaximum(std::max(10.0, radius));
    ui->mouseLookRadiusSpin->setValue(radius);
    ui->mouseLookRadiusSlider->setValue(qRound(std::min(radius, 2.0) * 100));
}
