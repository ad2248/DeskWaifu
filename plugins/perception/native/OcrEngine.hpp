#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <memory>
#include <stdexcept>
#include <vector>

// 携带稳定错误码的模型加载或兼容性异常，供模块转换为协议错误。
class OcrModelError : public std::runtime_error
{
public:
    OcrModelError(const char* code, const QString& message)
        : std::runtime_error(message.toUtf8().constData()), code(code) {}
    const char* code;
};

// 持有检测、识别 ONNX 会话与字典，完成单张图像的文字检测和逐行识别。
// 由 OCR 专用线程串行创建、调用和销毁。
class OcrEngine
{
public:
    // 加载并校验两个模型目录；缺失、格式不兼容或加载失败时抛出异常。
    OcrEngine(const QString& detectionModelDir, const QString& recognitionModelDir);
    // 同步读取绝对路径图像，返回尺寸、文字行及耗时；读取或推理失败时抛出异常。
    // 仅接受单帧、文件不超过 64 MiB 且每边不超过 4096 像素的图像。
    QJsonObject Recognize(const QString& image);

private:
    Ort::Value Run(Ort::Session& session, std::vector<float>& pixels, int height, int width);
    std::vector<std::vector<cv::Point2f>> Detect(const cv::Mat& image);
    QJsonObject ReadLine(const cv::Mat& image, const std::vector<cv::Point2f>& box);
    Ort::Env _env{ORT_LOGGING_LEVEL_ERROR, "deskwaifu-ocr"};
    Ort::SessionOptions _options;
    std::unique_ptr<Ort::Session> _detector, _recognizer;
    QJsonArray _characters;
    int _recognitionHeight = 48, _recognitionWidth = 320;
    float _threshold = 0.2f, _boxThreshold = 0.45f, _unclipRatio = 1.4f;
    size_t _maxCandidates = 3000;
};
