#pragma once

#include <QJsonObject>
#include <QString>

// Failure leaves the output object / existing destination file untouched.
bool ReadJsonObject(const QString& path, QJsonObject& object, QString* error = nullptr);
bool WriteJsonObject(const QString& path, const QJsonObject& object, QString* error = nullptr);
