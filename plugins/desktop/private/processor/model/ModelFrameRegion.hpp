#pragma once
#include "contracts/ModelRegion.hpp"
#include <vector>
// 在当前 GL 绘制目标清空裁剪区域外的像素，并恢复剪裁、颜色掩码和清除色。
void ClipModelRegion(ModelRegion region, int width, int height);
// 从指定帧缓冲读回 alpha 并生成裁剪后的输入区域；pixels 是可复用的输出缓冲。
// 需要当前 GL 上下文，返回前恢复原读帧缓冲绑定。
std::vector<ModelRegion> ReadModelInputRegion(unsigned int framebuffer, int width, int height, ModelRegion clip, std::vector<unsigned char>& pixels);
// 在当前 GL 目标绘制视口内的编辑手柄，并恢复修改过的剪裁和清除状态。
void DrawModelEditFrame(const std::vector<ModelRegion>& handles, int width, int height);
