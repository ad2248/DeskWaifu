#pragma once

#include "ModelCache.hpp"

// Activity is committed only after the requested model loads. Prefetch is
// best-effort and never invalidates the active model or reports success for a
// failed selection.
template<typename Model>
class ModelSession
{
public:
    // Soft budget: the currently active model is pinned even if it alone
    // exceeds the budget. Shared textures are conservatively counted once per
    // model here; actual GPU residency is tracked by TextureResidency.
    void SetTextureBudget(std::size_t bytes) { _textureBudget = bytes; }
    void Initialize(std::size_t count)
    {
        _cache.Initialize(count);
        _current = -1;
    }

    template<typename Load>
    bool Select(int index, Load load)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= _cache.Size()) return false;
        if (!_cache.Get(index))
        {
            auto model = load(index);
            if (!model) return false;
            _cache.Store(index, std::move(model));
        }
        _current = index;
        const std::size_t next = (static_cast<std::size_t>(index) + 1) % _cache.Size();
        _cache.EvictExcept(index, next);
        if (_cache.Size() > 1 && !_cache.Get(next)) _cache.Store(next, load(next));
        for (std::size_t candidate = 0; candidate < _cache.Size() &&
            _cache.ModelBytes() > _textureBudget; ++candidate)
            if (candidate != static_cast<std::size_t>(index) && candidate != next)
                _cache.Evict(candidate);
        if (_cache.ModelBytes() > _textureBudget && next != static_cast<std::size_t>(index))
            _cache.Evict(next);
        return true;
    }

    int CurrentIndex() const { return _current; }
    Model* Current() const { return _current < 0 ? nullptr : _cache.Get(_current); }
    ModelCache<Model>& Cache() { return _cache; }

    void Clear()
    {
        _cache.Clear();
        _current = -1;
    }

private:
    ModelCache<Model> _cache;
    int _current = -1;
    std::size_t _textureBudget = 128u * 1024u * 1024u;
};
