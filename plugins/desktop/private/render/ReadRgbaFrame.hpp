#pragma once
#include <vector>

// 为当前桌面 GL 上下文初始化 GLEW 并清除探测遗留错误；失败返回 false。
bool InitializeRenderGl();
// 从非零帧缓冲读回紧密排列、自下而上的 RGBA8 像素，覆盖 pixels。
// 需要当前 GL 上下文；恢复读帧缓冲及像素打包状态，参数无效或 GL 报错返回 false。
bool ReadRgbaFrame(unsigned framebuffer, int width, int height, std::vector<unsigned char>& pixels);
