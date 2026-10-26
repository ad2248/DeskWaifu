#pragma once
#include <QScrollArea>
#include <QJsonObject>
class GuiHostServices;
namespace Ui { class SpeechTab; }
class SpeechTab : public QScrollArea
{
public:
    explicit SpeechTab(GuiHostServices& services);
    ~SpeechTab() override;
    void start();
private:
    void save(const QString& key);
    void handleResponse(const QString& command, const QJsonObject& response);
    Ui::SpeechTab* ui;
    GuiHostServices& _client;
    bool _speechSetPending = false;
};
