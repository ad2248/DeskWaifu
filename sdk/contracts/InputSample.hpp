#pragma once
#include <QMetaType>
#include <memory>

// GUI 线程的有界连续采样槽；离散事件形成顺序屏障。
struct InputSample
{
    double x = 0, y = 0;
    bool cursor = false, consumed = false;
};
using InputSamplePtr = std::shared_ptr<InputSample>;
Q_DECLARE_METATYPE(InputSamplePtr)
