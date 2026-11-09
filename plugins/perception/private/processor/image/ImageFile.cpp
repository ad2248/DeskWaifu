#include "ImageFile.hpp"

ImageFilePtr CreateImageFile(const QImage& image)
{
    if (image.isNull() || image.width() > 4096 || image.height() > 4096) return {};
    ImageFilePtr file(new ImageFile);
    if (!image.save(file->Path(), "PNG")) return {};
    return file;
}
