#include "LogTab.hpp"
#include "ui_LogTab.h"
#include "plugin/GuiHostServices.hpp"

LogTab::LogTab(GuiHostServices& services) : ui(new Ui::LogTab)
{
    ui->setupUi(this);
    connect(&services, &GuiHostServices::logMessage, ui->responseLog, &QPlainTextEdit::appendPlainText);
    connect(&services, &GuiHostServices::protocolError, this, [&services](const QString& message) {
        services.publishLog(message);
        services.setStatusMessage(message, 5000);
    });
    connect(&services, &GuiHostServices::commandOutcomeUnknown, this, [this, &services](const QString& command) {
        const auto message = tr("连接断开：命令“%1”的执行结果未知，未重试该命令。").arg(command);
        services.publishLog(message);
        services.setStatusMessage(message, 8000);
    });
    connect(&services, &GuiHostServices::commandCancelled, this, [this, &services](const QString& command) {
        const auto message = tr("连接断开：排队中的命令“%1”已在发送前取消。").arg(command);
        services.publishLog(message);
        services.setStatusMessage(message, 8000);
    });
}
LogTab::~LogTab() { delete ui; }
