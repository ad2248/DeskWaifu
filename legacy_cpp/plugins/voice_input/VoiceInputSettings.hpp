#pragma once
#include "contracts/ConfigStore.hpp"
#include <QJsonObject>
#include <QString>

struct VoiceInputConfig {
    QString apiKey;
    QString endpoint = QStringLiteral("wss://llm-y3exskfcgxgxzn23.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference");
    QString model = QStringLiteral("qwen-audio-3.1-asr-flash-streaming");
    QString deviceId;
    int maxRecordingSeconds = 60;
};

class VoiceInputSettings {
public:
    explicit VoiceInputSettings(const QString& directory);
    bool Refresh(QString& error);
    bool Set(const std::string& rawJson, QString& error);
    VoiceInputConfig Snapshot() const { return _config; }
    QJsonObject PublicView() const;
    bool Configured() const { return !_config.apiKey.isEmpty(); }

private:
    static bool Decode(const ConfigStore::Snapshot&, VoiceInputConfig&);
    ConfigStore _store;
    VoiceInputConfig _config;
};
