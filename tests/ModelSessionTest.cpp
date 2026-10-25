#include "ModelSession.hpp"

#include <memory>
#include <type_traits>
#include <utility>

struct FakeModel {
    explicit FakeModel(int value) : value(value) {}
    int value;
    std::size_t TextureBytes() const { return 16; }
};

static_assert(std::is_same<decltype(std::declval<ModelSession<FakeModel>&>().Cache()),
    const ModelCache<FakeModel>&>::value, "Session callers must not mutate cache ownership");

int main()
{
    ModelSession<FakeModel> session;
    session.Initialize(5);
    int calls = 0;
    int activeLoads = 0;
    const auto load = [&](int index) -> std::unique_ptr<FakeModel>
    {
        ++calls;
        if (index == 3) ++activeLoads;
        if (index == 2) return nullptr;  // 损坏的模型，预取时也一样。
        return std::unique_ptr<FakeModel>(new FakeModel(index));
    };
    if (!session.Select(0, load) || session.CurrentIndex() != 0 || !session.Current()) return 1;
    if (session.Cache().Get(1) == nullptr || session.Cache().Get(2) != nullptr) return 1;
    // 只读缓存仍可借出可修改模型，以更新视口。
    const auto& view = session.Cache();
    for (std::size_t i = 0; i < view.Size(); ++i)
        if (auto* model = view.Get(i)) model->value += 10;
    if (session.Current()->value != 10 || view.Get(1)->value != 11) return 1;
    session.Current()->value = 0;
    view.Get(1)->value = 1;
    if (session.Select(2, load) || session.CurrentIndex() != 0 || session.Current()->value != 0) return 1;
    if (!session.Select(1, load) || session.CurrentIndex() != 1 || session.Current()->value != 1) return 1;
    if (session.Cache().Get(0) || session.Cache().Get(2)) return 1;
    if (!session.Select(3, load) || !session.Cache().Get(4)) return 1;
    if (session.Cache().Get(1)) return 1;
    session.SetTextureBudget(16); // 预算仅容纳活动模型。
    if (!session.Select(3, load) || session.Cache().Get(4) != nullptr ||
        session.Cache().ModelBytes() != 16) return 1;
    session.SetTextureBudget(8); // 活动模型超出软预算时仍被保留。
    auto* pinned = session.Current();
    const int beforeSelect = activeLoads;
    if (!pinned || session.Cache().ModelBytes() != 16) return 1;
    if (!session.Select(3, load) || session.Current() == nullptr ||
        session.Current() != pinned || activeLoads != beforeSelect ||
        session.Cache().ModelBytes() != 16) return 1;
    session.Clear();
    return session.CurrentIndex() == -1 && session.Current() == nullptr ? 0 : 1;
}
