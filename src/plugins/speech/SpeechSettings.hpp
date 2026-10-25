#pragma once

#include "contracts/ConfigStore.hpp"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <algorithm>

// 仅语音插件解释此配置；凭据通过 IPC 只写不读。
class SpeechSettings
{
public:
    explicit SpeechSettings(const QString& directory)
        : _store(QDir(directory).filePath(QStringLiteral("config.json")).toStdString(), true) {}

    bool Refresh(QString& error)
    {
        ConfigStore::Snapshot snapshot;
        std::string message, key;
        if (!_store.Read(snapshot, message) || !Decode(snapshot, key, message, false)) {
            error = QString::fromStdString(message); return false;
        }
        _apiKey = QString::fromStdString(key);
        error.clear(); return true;
    }

    bool Set(const QJsonObject& request, QString& error)
    { return Set(QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString(), error); }

    // 未验证的 JSON 必须在转为 QJsonObject（会折叠重复键）前走此入口。
    bool Set(const std::string& json, QString& error)
    {
        ConfigStore::Snapshot request, latest, saved;
        std::string message, key, previous;
        if (!ConfigStore::Parse(json, request, message, false) || !Decode(request, key, message, true) ||
            !_store.Read(latest, message) || !Decode(latest, previous, message, false)) {
            error = QString::fromStdString(message); return false;
        }
        auto proposed = latest;
        proposed.values["api_key"] = request.values.at("api_key");
        std::string document = "{";
        for (const auto& field : proposed.values) {
            if (document.size() > 1) document += ',';
            const auto name = QJsonDocument(QJsonArray{QString::fromStdString(field.first)}).toJson(QJsonDocument::Compact);
            document += name.mid(1, name.size() - 2).toStdString() + ':' + field.second;
        }
        document += "}\n";
        if (!_store.Commit(document, latest, saved, message)) {
            error = QString::fromStdString(message); return false;
        }
        _apiKey = QString::fromStdString(key);
        error.clear(); return true;
    }

    QString ApiKey() const { return _apiKey; }
    bool Configured() const { return !_apiKey.isEmpty(); }

private:
    static bool Decode(const ConfigStore::Snapshot& snapshot, std::string& key, std::string& error, bool required)
    {
        const auto value = snapshot.values.find("api_key");
        if (value == snapshot.values.end() && !required) { key.clear(); return true; }
        if (value == snapshot.values.end() || !ConfigStore::ParseString(value->second, key) || key.size() > 4096 ||
            std::any_of(key.begin(), key.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
            error = "Invalid MiniMax API Key"; return false;
        }
        return true;
    }

    ConfigStore _store;
    QString _apiKey;
};
