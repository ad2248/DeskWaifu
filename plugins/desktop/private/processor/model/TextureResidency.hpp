#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>

// 租约借用资源表；组装入口必须先销毁全部模型，再销毁纹理存储。
// 没有活动借用者时不保留纹理。
template<typename Id>
class TextureResidency
{
public:
    using Destroy = std::function<void(Id)>;
    explicit TextureResidency(Destroy destroy) : _destroy(std::move(destroy)) {}
    TextureResidency(const TextureResidency&) = delete;
    TextureResidency& operator=(const TextureResidency&) = delete;

    class Lease
    {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept { *this = std::move(other); }
        Lease& operator=(Lease&& other) noexcept
        {
            if (this != &other) {
                Reset();
                _owner = other._owner;
                _key = std::move(other._key);
                _id = other._id;
                _bytes = other._bytes;
                other._owner = nullptr;
            }
            return *this;
        }
        ~Lease() { Reset(); }
        explicit operator bool() const { return _owner != nullptr; }
        Id IdValue() const { return _id; }
        std::size_t Bytes() const { return _bytes; }
        const std::string& Key() const { return _key; }
        void Reset()
        {
            if (_owner) { _owner->Release(_key); _owner = nullptr; }
        }
    private:
        friend class TextureResidency;
        Lease(TextureResidency* owner, std::string key, Id id, std::size_t bytes)
            : _owner(owner), _key(std::move(key)), _id(id), _bytes(bytes) {}
        TextureResidency* _owner = nullptr;
        std::string _key;
        Id _id{};
        std::size_t _bytes = 0;
    };

    template<typename Create>
    Lease Acquire(const std::string& key, Create create)
    {
        if (key.empty()) return {};
        auto found = _entries.find(key);
        if (found == _entries.end()) {
            auto resource = create(key); // 返回 pair<Id, bytes>，ID 为零表示失败。
            if (!resource.first) return {};
            found = _entries.emplace(key, Entry{resource.first, resource.second, 0}).first;
            _bytes += resource.second;
        }
        ++found->second.refs;
        return Lease(this, key, found->second.id, found->second.bytes);
    }

    std::size_t ResidentBytes() const { return _bytes; }
    std::size_t ResidentCount() const { return _entries.size(); }

private:
    struct Entry { Id id; std::size_t bytes; std::size_t refs; };
    void Release(const std::string& key)
    {
        auto found = _entries.find(key);
        if (found == _entries.end() || --found->second.refs != 0) return;
        _bytes -= found->second.bytes;
        _destroy(found->second.id);
        _entries.erase(found);
    }
    Destroy _destroy;
    std::map<std::string, Entry> _entries;
    std::size_t _bytes = 0;
};
