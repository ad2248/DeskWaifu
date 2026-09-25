#pragma once

#include "ModelFrameInput.hpp"
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

// Per-channel arbitration. A live stream takes precedence over a persistent
// command; within a tier the most recently updated source wins. Releasing a
// source reveals the next source, rather than erasing unrelated controls.
class ExternalControlState
{
public:
    enum class Lifetime { Persistent, Stream };

    bool SetMouth(const std::string& id, Lifetime lifetime, float value)
    {
        if (id.empty() || !std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
        auto& source = _sources[id];
        source.lifetime = lifetime;
        source.mouth = {true, value, 0.0f, ++_order};
        return true;
    }

    bool SetLook(const std::string& id, Lifetime lifetime, float x, float y)
    {
        if (id.empty() || !std::isfinite(x) || !std::isfinite(y)) return false;
        auto& source = _sources[id];
        source.lifetime = lifetime;
        source.look = {true, x, y, ++_order};
        return true;
    }

    void ResetMouth(const std::string& id) { auto it = _sources.find(id); if (it != _sources.end()) it->second.mouth.active = false; }
    void ResetLook(const std::string& id) { auto it = _sources.find(id); if (it != _sources.end()) it->second.look.active = false; }
    void Release(const std::string& id) { _sources.erase(id); }
    void Clear() { _sources.clear(); }

    ModelFrameInput Frame() const
    {
        ModelFrameInput result;
        const Source* mouth = nullptr;
        const Source* look = nullptr;
        for (const auto& entry : _sources)
        {
            const Source& source = entry.second;
            if (source.mouth.active && (!mouth || Higher(source, *mouth, true))) mouth = &source;
            if (source.look.active && (!look || Higher(source, *look, false))) look = &source;
        }
        if (mouth) { result.hasMouth = true; result.mouth = mouth->mouth.x; }
        if (look) {
            result.hasLook = true;
            result.lookX = look->look.x;
            result.lookY = look->look.y;
        }
        return result;
    }

private:
    struct Channel { bool active = false; float x = 0, y = 0; std::uint64_t order = 0; };
    struct Source { Lifetime lifetime = Lifetime::Persistent; Channel mouth, look; };
    static bool Higher(const Source& a, const Source& b, bool mouth)
    {
        if (a.lifetime != b.lifetime) return a.lifetime == Lifetime::Stream;
        return (mouth ? a.mouth.order : a.look.order) > (mouth ? b.mouth.order : b.look.order);
    }
    std::map<std::string, Source> _sources;
    std::uint64_t _order = 0;
};
