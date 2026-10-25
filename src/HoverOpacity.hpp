#pragma once

#include "contracts/ModelInputRegion.hpp"

class HoverOpacity
{
public:
    void Enter(double x, double y) { _inside = true; Move(x, y); }
    void Move(double x, double y) { _x = x; _y = y; }
    void Leave() { _inside = false; }

    float Resolve(bool enabled, float opacity, const std::vector<ModelRegion>& modelRegions,
        const std::vector<ModelRegion>& handles = {}, float idleOpacity = 1.0f) const
    {
        return enabled && _inside && ModelInputRegionContains(modelRegions, _x, _y)
            && !ModelInputRegionContains(handles, _x, _y) ? opacity : idleOpacity;
    }

private:
    bool _inside = false;
    double _x = 0, _y = 0;
};
