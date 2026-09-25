#pragma once

#include <cstddef>

// Cubism framework startup is CPU-only. Binding is necessary only for
// objects that may own GL resources.
inline bool HasGpuResources(const void* view, const void* manager,
    std::size_t residentTextureCount)
{
    return view || manager || residentTextureCount;
}

// Never invoke a GPU destructor without the context which created its
// resources current on this thread. If it cannot be restored, keep the
// objects and context for process-level cleanup rather than issue invalid GL.
template<typename Bind, typename Dispose>
bool ReleaseGpuResources(bool hasGpuResources, Bind bind, Dispose dispose)
{
    if (hasGpuResources && !bind()) return false;
    dispose();
    return true;
}
