#pragma once

#include <QJsonObject>
#include <QScrollArea>
#include <QTimer>

class GuiHostServices;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// GUI-thread page; services is borrowed from the plugin context.
class VoiceInputTab final : public QScrollArea {
public:
    explicit VoiceInputTab(GuiHostServices& services);
    ~VoiceInputTab() override;
    void start();
    // Stops GUI activity only; closing the page must not cancel backend recording.
    void stop();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    bool canPoll() const;
    void refresh();
    void poll();
    void save(bool clearKey);
    void mutate(const QString& command, const QJsonObject& parameters = { });
    void handleResponse(const QString& command, const QJsonObject& response);
    void updateControls();
    void cancelUnsent();

    GuiHostServices& _client;
    QTimer _pollTimer;
    bool _started = false;
    bool _pollingVisible = true;
    bool _settingsPending = false;
    bool _statusPending = false;
    bool _statusKnown = false;
    bool _discardStatus = false;
    bool _devicesPending = false;
    QString _mutation;
    QString _state = QStringLiteral("idle");
    QString _phase = QStringLiteral("idle");
    QString _sessionId;
    QString _resultRequested;
    QString _resultPending;
    QString _resultSession;
    bool _configured = false;
    QLineEdit* _endpoint;
    QLineEdit* _apiKey;
    QLineEdit* _model;
    QComboBox* _device;
    QSpinBox* _maxSeconds;
    QPushButton* _save;
    QPushButton* _clearKey;
    QPushButton* _start;
    QPushButton* _stop;
    QPushButton* _cancel;
    QPushButton* _retryCopy;
    QLabel* _settingsStatus;
    QLabel* _status;
    QLabel* _copyStatus;
    QPlainTextEdit* _result;
};
