#pragma once
#include <QScrollArea>
#include <QJsonObject>

class GuiHostServices;
class HotkeyPanel;
namespace Ui { class InteractionTab; }

class InteractionTab : public QScrollArea
{
public:
    explicit InteractionTab(GuiHostServices& services);
    ~InteractionTab() override;
    void start();
private:
    void refreshAll();
    void handleResponse(const QString& command, const QJsonObject& response);
    void requestHotkeys();
    void requestHoverOpacity();
    void setHoverOpacity(const QJsonObject& parameters);
    void showHoverOpacity(const QJsonObject& response);
    void requestMouseLook();
    void setMouseLook(double radius);
    void showMouseLook(double radius);
    void setStatusMessage(const QString& message, int timeout = 0);
    void logResponse(const QString& command, const QJsonObject& response);
    Ui::InteractionTab* ui;
    GuiHostServices& _client;
    HotkeyPanel* _hotkeys;
    QString _hotkeyModelToken;
    QJsonObject _lastHotkeyResponse;
    bool _hotkeyQueryPending = false;
    bool _hoverQueryPending = false;
    bool _hoverSetPending = false;
    bool _hoverSupported = false;
    QJsonObject _lastHoverResponse;
    bool _mouseLookQueryPending = false;
    bool _mouseLookSetPending = false;
    bool _mouseLookSupported = false;
    double _lastMouseLookRadius = 0.5;
};
