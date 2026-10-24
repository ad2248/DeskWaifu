#pragma once

#include <QImage>
#include <QMetaType>
#include <QSharedPointer>
#include <QTemporaryDir>

// A private temporary PNG survives while any queued event/service retains it.
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
