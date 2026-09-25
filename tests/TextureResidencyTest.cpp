#include "TextureResidency.hpp"
#include "GpuTeardown.hpp"
#include "RuntimeLifecycle.hpp"

#include <cassert>
#include <memory>
#include <vector>

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
    // The loader may fail after its first texture; destroying the partial
    // model must relinquish that lease without harming a shared borrower.
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
    // A failed bind must never invoke GPU deletion or release the context.
    if (ReleaseGpuResources(true, [] { return false; }, release) || !order.empty() || deletes != 1) return 1;
    if (!ReleaseGpuResources(true, [&] { order.push_back(0); return current; }, release)) return 1;
    if (order != std::vector<int>({0, 1, 2, 3, 4, 5, 6}) || deletes != 2 || current) return 1;
    // Before EGL initialization, no GPU resources exist, so partial teardown
    // must not attempt eglMakeCurrent.
    bool touched = false;
    if (!ReleaseGpuResources(false, [&] { touched = true; return false; }, [] {}) || touched) return 1;
    return 0;
}
