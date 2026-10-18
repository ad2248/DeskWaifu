#pragma once
#include "ModelRegion.hpp"
#include <vector>
void ClipModelRegion(ModelRegion region, int width, int height);
std::vector<ModelRegion> ReadModelInputRegion(unsigned int framebuffer, int width, int height,
    ModelRegion clip, std::vector<unsigned char>& pixels);
void DrawModelEditFrame(const std::vector<ModelRegion>& handles, int width, int height);
