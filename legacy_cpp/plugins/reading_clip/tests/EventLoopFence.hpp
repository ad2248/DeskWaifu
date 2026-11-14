#pragma once
#include "processor/Processor.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

// 向循环投递栅栏并处理 Qt 事件，最多等待 5 秒；投递失败或超时返回 false。
// 在循环所属线程调用，仅保证栅栏已被消费，不保证异步后台任务全部完成。
inline bool AwaitProcessorEvents(ProcessorEventLoop& loop)
{
    class Fence final : public Processor {
    public:
        using Processor::Processor;
        bool done = false;
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.receiver != this || event.type != "test.fence") {
                return false;
            }
            done = true;
            return true;
        }
    } fence(loop);
    if (!loop.Post({ "test.fence", &fence, &fence })) {
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while (!fence.done && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return fence.done;
}
