#pragma once

#include "ProcessorEvent.hpp"
#include <QObject>
#include <deque>
#include <mutex>
#include <memory>
#include <vector>

// Qt 提供等待和唤醒机制。此对象在所属 GUI 线程上按顺序、非递归地派发事件，
// 不另行实现 exec 循环。
class ProcessorEventLoop : public QObject
{
public:
    explicit ProcessorEventLoop(QObject* parent = nullptr) : QObject(parent) {}
    bool Register(IProcessor* processor);
    void Unregister(IProcessor* processor);
    bool Post(ProcessorEvent event); // 线程安全；指针生命周期仍由生产者负责。
    bool Contains(IProcessor* processor) const;

protected:
    bool event(QEvent* event) override;

private:
    struct Registration { IProcessor* processor; bool active = true; };
    bool ValidLocked(const ProcessorEvent& event) const;
    void WakeLocked();
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<Registration>> _processors;
    std::deque<ProcessorEvent> _events;
    bool _scheduled = false, _dispatching = false;
};
