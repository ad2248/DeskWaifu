#pragma once
#include "processor/Processor.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

inline bool AwaitProcessorEvents(ProcessorEventLoop& loop)
{
    class Fence final : public Processor
    {
    public:
        using Processor::Processor;
        bool done = false;
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.receiver != this || event.type != "test.fence") return false;
            done = true; return true;
        }
    } fence(loop);
    if (!loop.Post({"test.fence", &fence, &fence})) return false;
    QElapsedTimer timer; timer.start();
    while (!fence.done && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return fence.done;
}
