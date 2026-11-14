#pragma once

#include "contracts/ImageFile.hpp"
#include <QImage>

// 将图像编码为共享持有的临时 PNG，最后一个持有者释放时删除文件。
// 空图像、任一边超过 4096 像素或编码失败时返回空指针；可在工作线程调用。
ImageFilePtr CreateImageFile(const QImage& image);
