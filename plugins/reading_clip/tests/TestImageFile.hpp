#pragma once
#include "contracts/ImageFile.hpp"
#include <QImage>

// 将测试图像保存为临时 PNG；失败返回空指针，文件由共享持有者控制生命周期。
inline ImageFilePtr MakeTestImageFile(const QImage& image)
{
    auto file = ImageFilePtr::create();
    return image.save(file->Path(), "PNG") ? file : ImageFilePtr { };
}
