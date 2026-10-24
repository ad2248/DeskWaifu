#pragma once

#include "ProcessorEvent.hpp"
#include <QObject>
#include <deque>
#include <mutex>
#include <memory>
#include <vector>

// Qt supplies the waiting/wakeup mechanism. This object supplies ordered,
// non-recursive dispatch on its owning (GUI) thread; it is not another exec loop.
class ProcessorEventLoop : public QObject
{
public:
    explicit ProcessorEventLoop(QObject* parent = nullptr) : QObject(parent) {}
    bool Register(IProcessor* processor);
    void Unregister(IProcessor* processor);
    bool Post(ProcessorEvent event); // Thread-safe; pointer lifetimes remain the producer's responsibility.
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
