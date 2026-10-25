#include "processor/model/ModelCache.hpp"

#include <memory>

struct FakeModel
{
    explicit FakeModel(int& destroyed) : destroyed(destroyed) {}
    ~FakeModel() { ++destroyed; }
    int& destroyed;
    std::size_t TextureBytes() const { return 1; }
};

int main()
{
    int destroyed = 0;
    ModelCache<FakeModel> cache;
    cache.Initialize(5);
    for (int index = 0; index < 5; ++index)
        cache.Store(index, std::unique_ptr<FakeModel>(new FakeModel(destroyed)));
    cache.EvictExcept(2, 3);
    if (destroyed != 3 || cache.Get(2) == nullptr || cache.Get(3) == nullptr || cache.Get(0)) return 1;
    cache.EvictExcept(3, 4);
    if (destroyed != 4 || cache.Get(3) == nullptr) return 1;
    cache.Clear();
    if (destroyed != 5 || cache.Size() != 0) return 1;

    cache.Initialize(2);
    cache.Store(0, std::unique_ptr<FakeModel>(new FakeModel(destroyed)));
    cache.Store(1, std::unique_ptr<FakeModel>(new FakeModel(destroyed)));
    cache.EvictExcept(1, 0);
    return cache.Get(0) && cache.Get(1) ? 0 : 1;
}
