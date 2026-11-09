#include "ConfigJson.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace {
bool Fail(QString* error, const QString& text)
{ if (error) *error = text; return false; }
}

bool ReadJsonObject(const QString& path, QJsonObject& object, QString* error)
{
    if (error) error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Fail(error, file.errorString());
    const auto data = file.readAll();
    if (file.error() != QFileDevice::NoError) return Fail(error, file.errorString());
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) return Fail(error, parseError.errorString());
    if (!document.isObject()) return Fail(error, QStringLiteral("Expected a JSON object"));
    object = document.object();
    return true;
}

bool WriteJsonObject(const QString& path, const QJsonObject& object, QString* error)
{
    if (error) error->clear();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return Fail(error, QStringLiteral("Cannot create configuration directory"));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return Fail(error, file.errorString());
    const auto data = QJsonDocument(object).toJson();
    if (file.write(data) != data.size()) return Fail(error, file.errorString());
    if (!file.commit()) return Fail(error, file.errorString());
    return true;
}
