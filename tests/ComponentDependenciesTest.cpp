#include "processor/model/ResourcePath.hpp"
#include "support/RuntimeLifecycle.hpp"

#include <iostream>
#include <vector>

int main()
{
    if (ResolveResourcePath("/app/", "shader.glsl") != "/app/shader.glsl" ||
        ResolveResourcePath("/app/", "/models/a.model3.json") != "/models/a.model3.json")
        return 1;

    // 初始化可能在创建视图前终止；清理时仍须保持服务存活，
    // 直到所有借用者均已释放。
    std::vector<int> order;
    ReleaseRuntimeInOrder(
        [&] { order.push_back(1); }, [&] { order.push_back(2); },
        [&] { order.push_back(3); }, [&] { order.push_back(4); },
        [&] { order.push_back(5); }, [&] { order.push_back(6); });
    if (order != std::vector<int>({1, 2, 3, 4, 5, 6})) return 1;
    order.clear();
    ReleaseRuntimeInOrder(
        [&] { order.push_back(1); }, [] {}, [] {},
        [&] { order.push_back(4); }, [] {}, [&] { order.push_back(6); });
    if (order != std::vector<int>({1, 4, 6})) return 1;
    std::cout << "explicit component inputs and paths passed\n";
}
