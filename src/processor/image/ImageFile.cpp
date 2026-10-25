#include "ImageFile.hpp"

ImageFilePtr ImageFile::Create(const QImage& image)
{
    if (image.isNull() || image.width() > 4096 || image.height() > 4096) return {};
    ImageFilePtr file(new ImageFile);
    if (!file->_directory.isValid() || !image.save(file->Path(), "PNG")) return {};
    return file;
}
