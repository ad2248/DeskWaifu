#include "MainWindow.hpp"
#include "ControlHostServices.hpp"
#include "ui_MainWindow.h"
#include "plugin/GuiPluginManager.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QDebug>

#ifndef DESKWAIFU_PLUGIN_DIR
#define DESKWAIFU_PLUGIN_DIR "/usr/lib/deskwaifu/plugins"
#endif

namespace {
QString PluginDirectory()
{
    // An explicit empty override disables discovery, just like plugin_dir.
    if (qEnvironmentVariableIsSet("DESKWAIFU_CONTROL_PLUGIN_DIR"))
        return qEnvironmentVariable("DESKWAIFU_CONTROL_PLUGIN_DIR");
    const auto configDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QFile config(QDir(configDirectory).filePath(QStringLiteral("deskwaifu/config.json")));
    if (config.exists()) {
        if (!config.open(QIODevice::ReadOnly)) {
            qWarning() << "Cannot read plugin_dir:" << config.errorString();
            return {};
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(config.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            qWarning() << "Invalid control plugin configuration";
            return {};
        }
        const auto value = document.object().value(QStringLiteral("plugin_dir"));
        if (!value.isUndefined()) {
            if (!value.isString() || value.toString().contains(QChar::Null)) {
                qWarning() << "Invalid plugin_dir";
                return {};
            }
            auto path = value.toString();
            if (path == QStringLiteral("~")) path = QDir::homePath();
            else if (path.startsWith(QStringLiteral("~/"))) path.replace(0, 1, QDir::homePath());
            return path;
        }
    }
    return QStringLiteral(DESKWAIFU_PLUGIN_DIR);
}
}

MainWindow::MainWindow(QWidget* parent, const QString& procDirectory)
    : QMainWindow(parent), ui(new Ui::MainWindow),
      _services(new ControlHostServices(_client, *this))
{
    ui->setupUi(this);
    _services->setProperty("procDirectory", procDirectory);
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
    connect(&_statusMessageTimer, &QTimer::timeout, this, [this] { setStatusMessage({}); });
    connect(&_client, &DeskWaifuClient::connectionChanged, this,
        [this](bool, const QString& message) {
            _connectionStatusLabel->setText(message);
            _connectionStatusLabel->setToolTip(message);
        });
    connect(_services.get(), &GuiHostServices::backendReady, this, [this] {
        if (!_backendReadyEmitted) {
            _backendReadyEmitted = true;
            emit backendReady();
        }
    });
    connect(&_client, &DeskWaifuClient::protocolError, this,
        [this](const QString& message) { setStatusMessage(message, 5000); });
    _plugins.reset(new GuiPluginManager(PluginDirectory(), *_services, *ui->tabWidget));
    QString error;
    if (!_plugins->Start(&error)) {
        qWarning().noquote() << "[GUI plugins]" << error;
        setStatusMessage(error);
        emit _services->logMessage(error);
    }
    _client.connectToServer();
}

MainWindow::~MainWindow()
{
    _plugins->Stop();
}

void MainWindow::setStatusMessage(const QString& message, int timeoutMs)
{
    _statusMessageTimer.stop();
    _operationStatusLabel->setText(message);
    _operationStatusLabel->setToolTip(message);
    if (!message.isEmpty() && timeoutMs > 0) _statusMessageTimer.start(timeoutMs);
}
