// 定义输入管线共享的连续位置采样槽及 Qt 元类型。
#pragma once
#include <QMetaType>
#include <memory>

// GUI 线程的有界连续采样槽；离散事件形成顺序屏障。
// 生产者和消费者共享可变槽，消费前可覆盖坐标，消费后须新建槽；
// shared_ptr 仅管理生命周期，不提供跨线程读写同步。
struct InputSample {
    double x = 0, y = 0;
    bool cursor = false, consumed = false;
};
using InputSamplePtr = std::shared_ptr<InputSample>;
Q_DECLARE_METATYPE(InputSamplePtr)
