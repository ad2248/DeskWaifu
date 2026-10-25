#pragma once

#include "ApplicationService.hpp"

// 运行循环和无头测试共用：每个可见帧先重新应用缓存的鼠标视线，
// 再由模型应用外部覆盖值。
template<typename CursorUpdate, typename Render>
void RenderModelFrame(const ApplicationService& service, bool editing,
    CursorUpdate updateCursor, Render render)
{
    const auto state = service.Snapshot();
    if (state.hidden) return;
    if (!editing) updateCursor(state.transform);
    render(state.transform, service.FrameInput());
}
