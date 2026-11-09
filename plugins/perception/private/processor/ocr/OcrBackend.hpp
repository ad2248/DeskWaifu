#pragma once

#include <QJsonObject>
#include <QString>
#include <QtPlugin>

// Private, versioned interface. The optional module and host must use matching Qt/C++ ABIs.
// OCR 模块的同步后端；由宿主专用线程串行调用并负责销毁。
class OcrBackend
{
public:
    virtual ~OcrBackend() = default;
    // 加载指定目录的模型，返回含 ok 的结果；失败结果应包含 error 和错误码。
    virtual QJsonObject Initialize(const QString& detectionDirectory, const QString& recognitionDirectory) = 0;
    // 识别图像文件；成功返回 ok 和 result，失败返回 ok=false 和 error。
    virtual QJsonObject Recognize(const QString& image) = 0;
    // 在模块卸载前释放推理资源，不允许抛出异常。
    virtual void Release() noexcept = 0;
};

// 由 Qt 插件实例提供的后端工厂，生命周期受插件加载器管理。
class OcrBackendFactory
{
public:
    virtual ~OcrBackendFactory() = default;
    // 创建独立后端并将所有权交给宿主；宿主销毁后端后才可卸载模块。
    virtual OcrBackend* Create() = 0;
};

#define DeskWaifuOcrBackend_iid "org.deskwaifu.OcrBackendFactory/1.0"
Q_DECLARE_INTERFACE(OcrBackendFactory, DeskWaifuOcrBackend_iid)
