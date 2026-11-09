// 提供插件配置所用的 Qt JSON 对象读写辅助函数。
#pragma once

#include <QJsonObject>
#include <QString>

// 读取严格 JSON 对象，成功更新 object 并返回 true；失败不改变 object。
// 提供 error 时，成功清空，失败写入原因。
bool ReadJsonObject(const QString& path, QJsonObject& object, QString* error = nullptr);
// 创建所需目录并原子写入 JSON 对象；成功返回 true，失败保留已有目标文件。
// 禁止退回直接写入；提供 error 时，成功清空，失败写入原因。
// 原子替换不提供并发读改写事务，调用方须协调多个写入者。
bool WriteJsonObject(const QString& path, const QJsonObject& object, QString* error = nullptr);
