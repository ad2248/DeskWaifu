#include "processor/model/TextureResidency.hpp"
#include "processor/model/LAppTextureManager.hpp"

#include <iostream>
#include <memory>
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
    int uploads = 0, deletes = 0;
    TextureResidency<unsigned> textures([&](unsigned) { ++deletes; });
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

    // Keep production lease accounting. Actual context/thread cleanup and
    // partial initialization now run in model-render-thread, also in Release.
    third.Reset();
    if (textures.ResidentCount() != 0 || deletes != 2) {
        std::cerr << "last resident lease must delete exactly once\n"; return 1;
    }
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
