#pragma once
#include "processor/model/ModelProcessor.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>

inline QJsonObject AwaitModel(ModelProcessor& processor, QJsonObject operation = {{"command", "reset_drag"}})
{
    bool done = false;
    QJsonObject result;
    if (!processor.Submit(QJsonDocument(operation).toJson(QJsonDocument::Compact).toStdString(),
        [&](QJsonObject reply) { result = reply; done = true; })) return {};
    QElapsedTimer timeout; timeout.start();
    while (!done && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    // 断言失败后，任何回调都不能继续借用当前栈变量。
    if (!done) qFatal("Timed out waiting for model operation");
    return result;
}
