#pragma once
#include "ModelRegion.hpp"
struct WaylandContext;
void ClipWaylandModelRegion(ModelRegion region, int width, int height);
void UpdateWaylandInputRegion(WaylandContext* wl, ModelRegion region);
bool ClearWaylandInputRegion(WaylandContext* wl);
