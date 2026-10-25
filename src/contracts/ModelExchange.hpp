#pragma once

#include "RenderFrame.hpp"
#include <mutex>

// 唯一跨渲染线程的帧交换边界：覆盖最新值，不排队，不携带业务回调。
struct ModelExchange
{
    std::mutex mutex;
    ModelRenderState state;
    std::shared_ptr<const RenderFrame> frame;
    QString error;
};
