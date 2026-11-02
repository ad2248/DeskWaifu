#pragma once
#include <QScrollArea>
#include <QJsonObject>

class GuiHostServices;
namespace Ui { class ModelTab; }

class ModelTab : public QScrollArea
{
public:
    explicit ModelTab(GuiHostServices& services);
    ~ModelTab() override;
    void start();
private:
    void bindControls();
    void refreshAll();
    void handleResponse(const QString& command, const QJsonObject& response);
    void setStatusMessage(const QString& message, int timeout = 0);
    void logResponse(const QString& command, const QJsonObject& response);
    void requestTransform();
    void queueTransformPreview();
    QJsonObject transformParameters() const;
    void showTransform(const QJsonObject& response);
    void sendTransformSave(const QString& command, QJsonObject parameters = {});
    Ui::ModelTab* ui;
    GuiHostServices& _client;
    QString _currentModel;
    bool _transformQueryPending = false;
    bool _transformUpdating = false;
    bool _transformEditStarting = false;
    bool _transformOwned = false;
    bool _transformConflict = false;
    bool _transformPreviewPending = false;
    bool _transformPreviewInFlight = false;
    bool _transformSupported = false;
    bool _outputSwitchPending = false;
    int _surfaceWidth = 1, _surfaceHeight = 1;
    QJsonObject _lastTransformResponse;
    QString _transformSaveRequest;
};
