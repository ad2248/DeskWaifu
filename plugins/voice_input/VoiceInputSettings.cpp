#include "VoiceInputSettings.hpp"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <cmath>

VoiceInputSettings::VoiceInputSettings(const QString& directory)
    : _store(QDir(directory).filePath(QStringLiteral("config.json")).toStdString(), true)
{
}

bool VoiceInputSettings::Decode(const ConfigStore::Snapshot& snapshot, VoiceInputConfig& config)
{
    for (const auto& [name, raw] : snapshot.values) {
        if (name == "max_recording_seconds") {
            const auto array = QJsonDocument::fromJson(QByteArray::fromStdString("[" + raw + "]")).array();
            if (array.size() != 1 || !array[0].isDouble()) {
                return false;
            }
            const double value = array[0].toDouble();
            if (value < 1 || value > 300 || std::floor(value) != value) {
                return false;
            }
            config.maxRecordingSeconds = int(value);
        } else if (name == "api_key" || name == "endpoint" || name == "model" || name == "device_id") {
            std::string value;
            if (!ConfigStore::ParseString(raw, value) || value.size() > 4096) {
                return false;
            }
            const auto text = QString::fromStdString(value);
            if (name == "api_key") {
                for (const auto ch : text) {
                    if (ch.category() == QChar::Other_Control) {
                        return false;
                    }
                }
                config.apiKey = text;
            } else if (name == "endpoint") {
                // Empty resets the optional override to the reference endpoint.
                if (text.isEmpty()) {
                    config.endpoint = VoiceInputConfig { }.endpoint;
                } else {
                    const QUrl url(text, QUrl::StrictMode);
                    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || url.hasFragment() || url.authority().contains('@')) {
                        return false;
                    }
                    config.endpoint = text;
                }
            } else if (name == "model") {
                config.model = text.isEmpty() ? VoiceInputConfig { }.model : text;
            } else {
                config.deviceId = text;
            }
        }
    }
    return true;
}

bool VoiceInputSettings::Refresh(QString& error)
{
    ConfigStore::Snapshot snapshot;
    std::string diagnostic;
    VoiceInputConfig next;
    if (!_store.Read(snapshot, diagnostic) || !Decode(snapshot, next)) {
        error = QStringLiteral("Invalid or unreadable voice input configuration");
        return false;
    }
    _config = next;
    error.clear();
    return true;
}

bool VoiceInputSettings::Set(const std::string& rawJson, QString& error)
{
    ConfigStore::Snapshot request, latest, saved;
    std::string diagnostic;
    if (!ConfigStore::Parse(rawJson, request, diagnostic, false) || !_store.Read(latest, diagnostic)) {
        error = QStringLiteral("Invalid or unreadable voice input configuration");
        return false;
    }
    auto proposed = latest;
    for (const auto& [key, value] : request.values) {
        if (key != "command") {
            proposed.values[key] = value;
        }
    }
    VoiceInputConfig next;
    if (!Decode(proposed, next)) {
        error = QStringLiteral("Invalid voice input settings");
        return false;
    }
    std::string document = "{";
    for (const auto& [key, value] : proposed.values) {
        if (document.size() > 1) {
            document += ',';
        }
        const auto encoded = QJsonDocument(QJsonArray { QString::fromStdString(key) }).toJson(QJsonDocument::Compact);
        document += encoded.mid(1, encoded.size() - 2).toStdString() + ':' + value;
    }
    document += "}\n";
    if (!_store.Commit(document, latest, saved, diagnostic)) {
        error = QStringLiteral("Voice input configuration changed or could not be saved");
        return false;
    }
    _config = next;
    error.clear();
    return true;
}

QJsonObject VoiceInputSettings::PublicView() const
{
    return { { "configured", Configured() }, { "endpoint", _config.endpoint }, { "model", _config.model }, { "device_id", _config.deviceId },
        { "max_recording_seconds", _config.maxRecordingSeconds } };
}
