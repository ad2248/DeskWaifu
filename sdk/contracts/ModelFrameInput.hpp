// 定义模型逐帧更新所需的嘴型和视线输入契约。
#pragma once

// 每帧传给模型的不可变值。输入采集由组装入口负责，模型不依赖传输层。
// hasMouth 和 hasLook 表示对应输入是否有效；模型仅在有效时应用相应值。
struct ModelFrameInput
{
    bool hasMouth = false;
    float mouth = 0.0f;
    bool hasLook = false;
    float lookX = 0.0f;
    float lookY = 0.0f;
};
