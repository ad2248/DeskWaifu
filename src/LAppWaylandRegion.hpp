#pragma once
#include "ModelRegion.hpp"
#include <vector>
struct WaylandContext;
void ClipWaylandModelRegion(ModelRegion region, int width, int height);
void UpdateWaylandInputRegion(WaylandContext* wl, ModelRegion region,
    const std::vector<ModelRegion>& handles = {});
void DrawWaylandEditFrame(const std::vector<ModelRegion>& handles, int width, int height);
bool ClearWaylandInputRegion(WaylandContext* wl);
