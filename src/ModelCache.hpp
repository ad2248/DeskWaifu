#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// A session owns its cached models; callers receive borrowed pointers only.
// Current eviction policy is retained for compatibility; #81 owns GPU budget.
template<typename Model>
class ModelCache
{
public:
    void Initialize(std::size_t count)
    {
        _entries.clear();
        _entries.resize(count);
    }

    Model* Get(std::size_t index) const
    {
        return index < _entries.size() ? _entries[index].get() : nullptr;
    }

    void Store(std::size_t index, std::unique_ptr<Model> model)
    {
        if (index < _entries.size()) _entries[index] = std::move(model);
    }

    void EvictExcept(std::size_t current, std::size_t next)
    {
        if (_entries.size() <= 4) return;
        for (std::size_t index = 0; index < _entries.size(); ++index)
            if (index != current && index != next) _entries[index].reset();
    }

    void Clear() { _entries.clear(); }
    std::size_t Size() const { return _entries.size(); }
    void Evict(std::size_t index) { if (index < _entries.size()) _entries[index].reset(); }

    std::size_t ModelBytes() const
    {
        std::size_t bytes = 0;
        for (const auto& model : _entries) if (model) bytes += model->TextureBytes();
        return bytes;
    }

private:
    std::vector<std::unique_ptr<Model>> _entries;
};
