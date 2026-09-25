#include "ModelSession.hpp"

#include <memory>

struct FakeModel {
    explicit FakeModel(int value) : value(value) {}
    int value;
    std::size_t TextureBytes() const { return 16; }
};

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
        if (index == 2) return nullptr;  // broken model, including prefetch
        return std::unique_ptr<FakeModel>(new FakeModel(index));
    };
    if (!session.Select(0, load) || session.CurrentIndex() != 0 || !session.Current()) return 1;
    if (session.Cache().Get(1) == nullptr || session.Cache().Get(2) != nullptr) return 1;
    if (session.Select(2, load) || session.CurrentIndex() != 0 || session.Current()->value != 0) return 1;
    if (!session.Select(1, load) || session.CurrentIndex() != 1 || session.Current()->value != 1) return 1;
    if (session.Cache().Get(0) || session.Cache().Get(2)) return 1;
    if (!session.Select(3, load) || !session.Cache().Get(4)) return 1;
    if (session.Cache().Get(1)) return 1;
    session.SetTextureBudget(16); // only the active model fits
    if (!session.Select(3, load) || session.Cache().Get(4) != nullptr ||
        session.Cache().ModelBytes() != 16) return 1;
    session.SetTextureBudget(8); // active model is pinned above the soft budget
    auto* pinned = session.Current();
    const int beforeSelect = activeLoads;
    if (!pinned || session.Cache().ModelBytes() != 16) return 1;
    if (!session.Select(3, load) || session.Current() == nullptr ||
        session.Current() != pinned || activeLoads != beforeSelect ||
        session.Cache().ModelBytes() != 16) return 1;
    session.Clear();
    return session.CurrentIndex() == -1 && session.Current() == nullptr ? 0 : 1;
}
