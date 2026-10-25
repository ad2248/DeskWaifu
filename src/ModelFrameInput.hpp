#pragma once

// 每帧传给模型的不可变值。输入采集由组装入口负责，模型不依赖传输层。
struct ModelFrameInput
{
    bool hasMouth = false;
    float mouth = 0.0f;
    bool hasLook = false;
    float lookX = 0.0f;
    float lookY = 0.0f;
};
