#include "MainWindow.hpp"
#include "ui_MainWindow.h"

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
    bindControls();

    connect(&_client, &DeskWaifuClient::connectionChanged, this,
        [this](bool connected, const QString& message) {
            ui->connectionLabel->setText(connected ? tr("Connected") : tr("Disconnected"));
            ui->connectionLabel->setProperty("connected", connected);
            ui->connectionLabel->style()->unpolish(ui->connectionLabel);
            ui->connectionLabel->style()->polish(ui->connectionLabel);
            statusBar()->showMessage(message);
            if (connected)
            {
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
        _client.sendCommand(QStringLiteral("set_model_zoom"),
            {{QStringLiteral("value"), ui->zoomSpin->value()}});
        _client.sendCommand(QStringLiteral("set_model_position"), {
            {QStringLiteral("x"), ui->positionXSpin->value()},
            {QStringLiteral("y"), ui->positionYSpin->value()}
        });
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
    _client.sendCommand(QStringLiteral("get_motions"));
    _client.sendCommand(QStringLiteral("get_model_position"));
}

void MainWindow::handleResponse(const QString& command, const QJsonObject& response)
{
    logResponse(command, response);
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
