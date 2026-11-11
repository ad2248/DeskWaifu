#pragma once

#include <QStringList>

// 后端宿主启动配置；插件根目录与插件配置根目录相互独立。
struct HostConfig {
    QString pluginDirectory;
    QString configDirectory;
    // 从 --config 或默认配置文件加载目录，并应用 DESKWAIFU_PLUGIN_DIR 覆盖。
    // 成功返回 true；失败返回 false 并写入 error，此时 config 可能已被部分更新。
    static bool Load(const QStringList& arguments, HostConfig& config, QString& error);
};
