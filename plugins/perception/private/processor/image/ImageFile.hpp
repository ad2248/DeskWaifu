#pragma once

#include "contracts/ImageFile.hpp"
#include <QImage>

ImageFilePtr CreateImageFile(const QImage& image);
