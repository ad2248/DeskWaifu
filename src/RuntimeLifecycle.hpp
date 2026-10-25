#pragma once

// 传输层析构时会通知服务客户端已断开。
// 服务必须存活到这些回调全部完成。
template<typename Transport, typename Service>
void ReleaseCommandTransport(Transport transport, Service service)
{
    transport();
    service();
}

// 按顺序清理应用。部分初始化尚未到达的阶段可传入空操作。
// 任一对象析构时，其借用的后续阶段依赖都必须仍然存活。
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
