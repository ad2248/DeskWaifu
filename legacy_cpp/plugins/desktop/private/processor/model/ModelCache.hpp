#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// 会话持有缓存模型，调用方只获得借用指针。
// 不提供内部同步，访问和模型析构应在模型所属线程执行。
template <typename Model> class ModelCache {
public:
    void Initialize(std::size_t count)
    {
        _entries.clear();
        _entries.resize(count);
    }

    // 返回借用模型，空槽或越界返回空指针；替换、淘汰或清空后指针失效。
    Model* Get(std::size_t index) const { return index < _entries.size() ? _entries[index].get() : nullptr; }

    // 接管模型并替换原槽内容；越界时不缓存，传入模型随参数销毁。
    void Store(std::size_t index, std::unique_ptr<Model> model)
    {
        if (index < _entries.size()) {
            _entries[index] = std::move(model);
        }
    }

    void EvictExcept(std::size_t current, std::size_t next)
    {
        if (_entries.size() <= 4) {
            return;
        }
        for (std::size_t index = 0; index < _entries.size(); ++index) {
            if (index != current && index != next) {
                _entries[index].reset();
            }
        }
    }

    void Clear() { _entries.clear(); }
    std::size_t Size() const { return _entries.size(); }
    void Evict(std::size_t index)
    {
        if (index < _entries.size()) {
            _entries[index].reset();
        }
    }

    std::size_t ModelBytes() const
    {
        std::size_t bytes = 0;
        for (const auto& model : _entries) {
            if (model) {
                bytes += model->TextureBytes();
            }
        }
        return bytes;
    }

private:
    std::vector<std::unique_ptr<Model>> _entries;
};
