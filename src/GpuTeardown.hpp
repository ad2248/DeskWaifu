#pragma once

#include <cstddef>

// Cubism 框架启动只使用 CPU；仅可能持有 GL 资源的对象需要绑定上下文。
inline bool HasGpuResources(const void* view, const void* manager,
    std::size_t residentTextureCount)
{
    return view || manager || residentTextureCount;
}

// GPU 对象析构前，必须在当前线程绑定创建其资源的上下文。
// 无法恢复时，将对象和上下文留给进程退出时清理，避免发出无效 GL 调用。
template<typename Bind, typename Dispose>
bool ReleaseGpuResources(bool hasGpuResources, Bind bind, Dispose dispose)
{
    if (hasGpuResources && !bind()) return false;
    dispose();
    return true;
}
