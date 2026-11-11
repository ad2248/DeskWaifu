#pragma once
#include <QJsonObject>
#include <QScrollArea>
class GuiHostServices;
namespace Ui {
class SpeechTab;
}
// 提供语音凭据的保存、清除和配置状态查询，不回显或记录凭据。
// 在 GUI 线程使用；借用的 services 必须比页面存活更久。
class SpeechTab : public QScrollArea {
public:
    explicit SpeechTab(GuiHostServices& services);
    ~SpeechTab() override;
    // 已连接时启用保存入口并异步查询配置状态。
    void start();

private:
    void save(const QString& key);
    void handleResponse(const QString& command, const QJsonObject& response);
    Ui::SpeechTab* ui;
    GuiHostServices& _client;
    bool _speechSetPending = false;
};
