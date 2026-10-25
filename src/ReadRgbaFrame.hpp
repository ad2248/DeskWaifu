#pragma once
#include <vector>

bool InitializeRenderGl();
bool ReadRgbaFrame(unsigned framebuffer, int width, int height, std::vector<unsigned char>& pixels);
