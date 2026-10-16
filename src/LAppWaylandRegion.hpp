#pragma once
#include "ModelRegion.hpp"
#include <vector>
struct WaylandContext;
void ClipWaylandModelRegion(ModelRegion region, int width, int height);
std::vector<ModelRegion> ReadModelInputRegion(unsigned int framebuffer, int width, int height,
    ModelRegion clip, std::vector<unsigned char>& pixels);
void UpdateWaylandInputRegion(WaylandContext* wl, const std::vector<ModelRegion>& modelRegions,
    const std::vector<ModelRegion>& handles = {});
void DrawWaylandEditFrame(const std::vector<ModelRegion>& handles, int width, int height);
bool ClearWaylandInputRegion(WaylandContext* wl);
