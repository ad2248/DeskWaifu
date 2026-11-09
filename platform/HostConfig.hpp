#pragma once

#include <QStringList>

struct HostConfig
{
    QString pluginDirectory;
    QString configDirectory;
    static bool Load(const QStringList& arguments, HostConfig& config, QString& error);
};
