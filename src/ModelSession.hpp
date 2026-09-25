#pragma once

#include "ModelCache.hpp"

// Activity is committed only after the requested model loads. Prefetch is
// best-effort and never invalidates the active model or reports success for a
// failed selection.
template<typename Model>
class ModelSession
{
public:
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
};
