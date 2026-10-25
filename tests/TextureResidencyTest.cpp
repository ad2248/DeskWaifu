#include "processor/model/TextureResidency.hpp"
#include "support/GpuTeardown.hpp"
#include "support/RuntimeLifecycle.hpp"
#include "processor/model/LAppTextureManager.hpp"

#include <cassert>
#include <memory>
#include <vector>
#include <type_traits>

template<typename T, typename = void>
struct HasLegacyTextureCreation : std::false_type {};
template<typename T>
struct HasLegacyTextureCreation<T, decltype(void(std::declval<T&>().CreateTextureFromPngFile("x")))>
    : std::true_type {};
static_assert(!HasLegacyTextureCreation<LAppTextureManager>::value,
    "Texture creation must be owned by leases, not the legacy API");

int main()
{
    bool current = true;
    int uploads = 0, deletes = 0;
    TextureResidency<unsigned> textures([&](unsigned) { assert(current); ++deletes; });
    const auto upload = [&](const std::string& path)
    {
        ++uploads;
        return std::make_pair(path == "missing" ? 0u : static_cast<unsigned>(uploads), 32u);
    };
    auto first = textures.Acquire("shared", upload);
    auto second = textures.Acquire("shared", upload);
    if (!first || !second || first.IdValue() != second.IdValue() ||
        uploads != 1 || textures.ResidentBytes() != 32) return 1;
    auto third = textures.Acquire("other", upload);
    if (textures.ResidentBytes() != 64 || !third) return 1;
    first.Reset();
    if (deletes || textures.ResidentCount() != 2) return 1;
    second.Reset();
    if (deletes != 1 || textures.ResidentBytes() != 32) return 1;
    if (textures.Acquire("missing", upload)) return 1;
    // 加载器可能在加载首张纹理后失败；销毁部分构建的模型时，
    // 必须释放该租约，同时不影响共享借用者。
    {
        auto partial = textures.Acquire("other", upload);
        if (!partial || textures.ResidentCount() != 1 ||
            textures.Acquire("missing", upload)) return 1;
    }
    if (textures.ResidentCount() != 1 || deletes != 1) return 1;

    std::vector<int> order;
    const auto release = [&] {
        ReleaseRuntimeInOrder(
            [&] { order.push_back(1); }, [&] { order.push_back(2); },
            [&] { third.Reset(); order.push_back(3); },
            [&] { assert(textures.ResidentCount() == 0); order.push_back(4); },
            [&] { order.push_back(5); }, [&] { current = false; order.push_back(6); });
    };
    // 绑定失败时，不能删除 GPU 资源或释放上下文。
    if (ReleaseGpuResources(true, [] { return false; }, release) || !order.empty() || deletes != 1) return 1;
    if (!ReleaseGpuResources(true, [&] { order.push_back(0); return current; }, release)) return 1;
    if (order != std::vector<int>({0, 1, 2, 3, 4, 5, 6}) || deletes != 2 || current) return 1;
    // EGL 初始化前不存在 GPU 资源，因此部分清理不能尝试 eglMakeCurrent。
    bool touched = false;
    if (!ReleaseGpuResources(false, [&] { touched = true; return false; }, [] {}) || touched) return 1;

    // 单独启动 Cubism 只使用 CPU，不能请求绑定 EGL。
    if (HasGpuResources(nullptr, nullptr, 0)) return 1;
    const int gpuOwner = 0;
    if (!HasGpuResources(&gpuOwner, nullptr, 0) ||
        !HasGpuResources(nullptr, &gpuOwner, 0) ||
        !HasGpuResources(nullptr, nullptr, 1)) return 1;
    // Cubism 可能已启动，但尚未创建视图、管理器或纹理。
    bool cubismStarted = true, cleaned = false, bound = false;
    if (!ReleaseGpuResources(HasGpuResources(nullptr, nullptr, 0),
            [&] { bound = true; return false; },
            [&] { ReleaseRuntimeInOrder([] {}, [] {}, [] {}, [] {},
                [&] { cubismStarted = false; }, [&] { cleaned = true; }); }) ||
        bound || cubismStarted || !cleaned) return 1;
    int lastLeaseDeletes = 0;
    TextureResidency<unsigned> scoped([&](unsigned id) {
        if (id == 7) ++lastLeaseDeletes;
    });
    {
        auto owner = scoped.Acquire("texture", [](const std::string&) {
            return std::make_pair(7u, 4u);
        });
        auto borrower = scoped.Acquire("texture", [](const std::string&) {
            return std::make_pair(8u, 4u);
        });
        if (!owner || !borrower || lastLeaseDeletes) return 1;
    }
    if (lastLeaseDeletes != 1 || scoped.ResidentCount() != 0) return 1;
    return 0;
}
