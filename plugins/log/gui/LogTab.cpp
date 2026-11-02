#include "LogTab.hpp"
#include "ui_LogTab.h"
#include "plugin/GuiHostServices.hpp"

LogTab::LogTab(GuiHostServices& services) : ui(new Ui::LogTab)
{
    ui->setupUi(this);
    connect(&services, &GuiHostServices::logMessage, ui->responseLog, &QPlainTextEdit::appendPlainText);
}
LogTab::~LogTab() { delete ui; }
