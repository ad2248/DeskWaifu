#pragma once
#include "contracts/ImageFile.hpp"

inline ImageFilePtr MakeTestImageFile(const QImage& image)
{
    auto file = ImageFilePtr::create();
    return image.save(file->Path(), "PNG") ? file : ImageFilePtr{};
}
