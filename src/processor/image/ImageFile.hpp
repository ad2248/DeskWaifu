#pragma once

#include <QImage>
#include <QMetaType>
#include <QSharedPointer>
#include <QTemporaryDir>

// 只要排队事件或服务仍持有引用，私有临时 PNG 就保持有效。
class ImageFile
{
public:
    static QSharedPointer<ImageFile> Create(const QImage& image);
    QString Path() const { return _directory.filePath("image.png"); }
private:
    QTemporaryDir _directory;
};

using ImageFilePtr = QSharedPointer<ImageFile>;
Q_DECLARE_METATYPE(ImageFilePtr)
