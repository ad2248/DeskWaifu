#pragma once

#include "ModelCache.hpp"

// 仅在请求的模型加载完成后提交活动模型状态。预取尽力而为，
// 不会使当前模型失效，也不会将选择失败报告为成功。
template<typename Model>
class ModelSession
{
public:
    // 软预算：即使当前活动模型自身已超出预算，也要保留它。
    // 此处保守地对每个模型分别计入共享纹理；实际 GPU 驻留量由 TextureResidency 跟踪。
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
    const ModelCache<Model>& Cache() const { return _cache; }

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
