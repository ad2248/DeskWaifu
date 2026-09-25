#pragma once

// Ordered application teardown. Callers may pass no-op actions for stages
// not reached during partial initialization. No destructor may outlive a
// dependency borrowed from a later stage.
template<typename Stop, typename View, typename Models, typename Textures,
         typename Framework, typename Window>
void ReleaseRuntimeInOrder(Stop stop, View view, Models models,
    Textures textures, Framework framework, Window window)
{
    stop();
    view();
    models();
    textures();
    framework();
    window();
}
