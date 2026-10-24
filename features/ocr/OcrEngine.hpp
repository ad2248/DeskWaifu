#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <memory>
#include <vector>

class OcrEngine
{
public:
    explicit OcrEngine(const QString& models);
    QJsonObject Recognize(const QString& image);

private:
    Ort::Value Run(Ort::Session& session, std::vector<float>& pixels, int height, int width);
    std::vector<std::vector<cv::Point2f>> Detect(const cv::Mat& image);
    QJsonObject ReadLine(const cv::Mat& image, const std::vector<cv::Point2f>& box);
    Ort::Env _env{ORT_LOGGING_LEVEL_ERROR, "deskwaifu-ocr"};
    Ort::SessionOptions _options;
    std::unique_ptr<Ort::Session> _detector, _recognizer;
    QJsonArray _characters;
};
