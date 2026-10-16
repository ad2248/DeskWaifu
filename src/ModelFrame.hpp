#pragma once

#include "ApplicationService.hpp"

// Shared by the runtime loop and headless tests: reapply cached mouse gaze
// before every visible frame, then let the model apply external overrides.
template<typename CursorUpdate, typename Render>
void RenderModelFrame(const ApplicationService& service, bool editing,
    CursorUpdate updateCursor, Render render)
{
    const auto state = service.Snapshot();
    if (state.hidden) return;
    if (!editing) updateCursor(state.transform);
    render(state.transform, service.FrameInput());
}
