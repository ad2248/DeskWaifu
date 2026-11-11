// 提供跨排队事件共享的临时图片文件生命周期载体。
#pragma once

#include <QMetaType>
#include <QSharedPointer>
#include <QTemporaryDir>

// 只要排队事件或服务仍持有引用，私有临时 PNG 就保持有效。
class ImageFile {
public:
    // 返回临时目录中的目标 PNG 路径，不创建图片；目录创建失败时路径可能无效。
    QString Path() const { return _directory.filePath("image.png"); }

private:
    QTemporaryDir _directory;
};

using ImageFilePtr = QSharedPointer<ImageFile>;
Q_DECLARE_METATYPE(ImageFilePtr)
