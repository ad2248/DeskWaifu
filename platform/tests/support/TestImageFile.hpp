#pragma once
#include "contracts/ImageFile.hpp"
#include <QImage>

// 将图像写为 PNG 并返回共享的临时图像文件；保存失败返回空指针。
// 文件生命周期由返回的 ImageFilePtr 管理，供测试跨异步边界持有。
inline ImageFilePtr MakeTestImageFile(const QImage& image)
{
    auto file = ImageFilePtr::create();
    return image.save(file->Path(), "PNG") ? file : ImageFilePtr { };
}
