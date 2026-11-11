// 定义可多态共享的处理器事件及其只读所有权句柄。
#pragma once

#include <QString>
#include <QVariantMap>
#include <memory>
#include <utility>

class IProcessorPlugin;

// 可派生的处理器事件基类；type 用于路由，data 承载领域参数。
// sender 和 receiver 是不拥有的旧式处理器引用，空 receiver 表示按注册顺序派发。
// 投递后不应修改事件；裸指针的存活及注销顺序由生产者和处理器负责。
struct ProcessorEvent {
    ProcessorEvent(QString type = { }, IProcessorPlugin* sender = nullptr, IProcessorPlugin* receiver = nullptr, QVariantMap data = { })
        : type(std::move(type))
        , sender(sender)
        , receiver(receiver)
        , data(std::move(data))
    {
    }
    virtual ~ProcessorEvent() = default;

    QString type;
    IProcessorPlugin* sender = nullptr;
    IProcessorPlugin* receiver = nullptr;
    QVariantMap data;
};

// Share the dynamic event, never copy a base reference into a queue.
using ProcessorEventPtr = std::shared_ptr<const ProcessorEvent>;
