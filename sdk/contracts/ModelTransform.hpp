// 定义模型显示变换的数据契约，不依赖具体渲染实现。
#pragma once

// 模型显示变换的值对象，包含缩放及二维平移，不包含窗口像素坐标。
struct ModelTransform
{
    float scale;
    float x;
    float y;
};
