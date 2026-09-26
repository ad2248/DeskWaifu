#pragma once

// The transport's destructor notifies the service of client disconnects.
// Keep the service alive until those callbacks have completed.
template<typename Transport, typename Service>
void ReleaseCommandTransport(Transport transport, Service service)
{
    transport();
    service();
}

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
