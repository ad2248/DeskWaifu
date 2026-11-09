#pragma once
#include <QScrollArea>
#include <QJsonObject>

class GuiHostServices;
namespace Ui { class ModelTab; }

// 展示模型与输出设置，协调变换草稿的预览、保存及冲突反馈。
// 在 GUI 线程使用；借用的 services 必须比页面存活更久。
class ModelTab : public QScrollArea
{
public:
    explicit ModelTab(GuiHostServices& services);
    ~ModelTab() override;
    // 已连接时查询模型和变换状态；不阻塞等待后端回复。
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
