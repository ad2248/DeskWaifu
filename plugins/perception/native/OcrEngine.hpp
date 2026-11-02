#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <memory>
#include <stdexcept>
#include <vector>

class OcrModelError : public std::runtime_error
{
public:
    OcrModelError(const char* code, const QString& message)
        : std::runtime_error(message.toUtf8().constData()), code(code) {}
    const char* code;
};

class OcrEngine
{
public:
    OcrEngine(const QString& detectionModelDir, const QString& recognitionModelDir);
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
