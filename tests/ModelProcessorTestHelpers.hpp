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
    // No callback may borrow our stack after a failed assertion.
    if (!done) qFatal("Timed out waiting for model operation");
    return result;
}
