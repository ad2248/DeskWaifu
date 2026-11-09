#pragma once

#include <QImage>
#include <QMimeData>

inline QImage ClipboardImageFromMimeData(const QMimeData* mime)
{
    if (!mime) return {};
    if (mime->hasImage()) {
        const auto image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) return image;
    }
    for (const auto& format : mime->formats()) {
        if (format.startsWith("image/")) {
            const auto image = QImage::fromData(mime->data(format));
            if (!image.isNull()) return image;
        }
    }
    return {};
}
