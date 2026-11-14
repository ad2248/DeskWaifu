#pragma once

#include <QImage>
#include <QMimeData>

// 优先读取原生图像，再尝试解码 image/* 数据；无可用图像时返回空 QImage。
// 不接管 mime 所有权；访问系统剪贴板提供的对象时应在 GUI 线程调用。
inline QImage ClipboardImageFromMimeData(const QMimeData* mime)
{
    if (!mime) {
        return { };
    }
    if (mime->hasImage()) {
        const auto image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            return image;
        }
    }
    for (const auto& format : mime->formats()) {
        if (format.startsWith("image/")) {
            const auto image = QImage::fromData(mime->data(format));
            if (!image.isNull()) {
                return image;
            }
        }
    }
    return { };
}
