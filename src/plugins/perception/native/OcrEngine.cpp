// OCR preprocessing, DB quad and CTC pipeline follow PaddleOCR (PaddlePaddle Authors).
// See LICENSE-PaddleOCR and NOTICE for attribution and the float-unclip difference.
#include "OcrEngine.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <yaml-cpp/yaml.h>
#include <opencv2/imgproc.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry/2d.hpp>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
YAML::Node Metadata(const QString& directory, const char* modelName)
{
    const auto path = QDir(directory).filePath("inference.yml");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw OcrModelError("model_missing", "Cannot read OCR metadata: " + path);
    if (file.size() > 4 * 1024 * 1024)
        throw OcrModelError("model_incompatible", "OCR metadata exceeds 4 MiB: " + path);
    try {
        auto metadata = YAML::Load(file.readAll().toStdString());
        if (metadata["Global"]["model_name"].as<std::string>() != modelName)
            throw OcrModelError("model_incompatible", "Expected " + QString::fromLatin1(modelName) + " metadata: " + path);
        return metadata;
    } catch (const YAML::Exception& error) {
        throw OcrModelError("model_incompatible", "Invalid OCR metadata " + path + ": " + QString::fromUtf8(error.what()));
    }
}

YAML::Node Transform(const YAML::Node& metadata, const char* name)
{
    const auto transforms = metadata["PreProcess"]["transform_ops"];
    if (transforms.IsSequence())
        for (const auto& transform : transforms)
            if (transform[name]) return transform[name];
    throw OcrModelError("model_incompatible", "Missing preprocessing transform: " + QString::fromLatin1(name));
}

void Require(bool condition, const char* message)
{
    if (!condition) throw OcrModelError("model_incompatible", QString::fromLatin1(message));
}

void ValidateSession(Ort::Session& session, bool detection, int recognitionHeight, int characters)
{
    Require(session.GetInputCount() == 1 && session.GetOutputCount() == 1, "Expected one ONNX input and output");
    const auto inputType = session.GetInputTypeInfo(0);
    const auto outputType = session.GetOutputTypeInfo(0);
    Require(inputType.GetONNXType() == ONNX_TYPE_TENSOR && outputType.GetONNXType() == ONNX_TYPE_TENSOR,
            "Expected ONNX tensor input and output");
    const auto input = inputType.GetTensorTypeAndShapeInfo();
    const auto output = outputType.GetTensorTypeAndShapeInfo();
    Require(input.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            output.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, "Expected float32 ONNX tensors");
    const auto in = input.GetShape(), out = output.GetShape();
    const auto matches = [](int64_t dimension, int64_t expected) { return dimension == -1 || dimension == expected; };
    Require(in.size() == 4 && matches(in[0], 1) && matches(in[1], 3) && in[3] == -1 &&
            (detection ? in[2] == -1 : matches(in[2], recognitionHeight)),
            "Unsupported ONNX input shape; expected dynamic spatial detector or [1,3,48,dynamic] recognizer");
    if (detection)
        Require(out.size() == 4 && matches(out[0], 1) && matches(out[1], 1) &&
                out[2] != 0 && out[3] != 0, "Unsupported detector output shape");
    else
        Require(out.size() == 3 && matches(out[0], 1) && out[1] != 0 && matches(out[2], characters),
                "Recognizer output classes do not match inference.yml character dictionary");
}

std::unique_ptr<Ort::Session> LoadSession(Ort::Env& env, Ort::SessionOptions& options, const QString& directory)
{
    const auto path = QDir(directory).filePath("inference.onnx");
    if (!QFileInfo(path).isFile() || !QFileInfo(path).isReadable())
        throw OcrModelError("model_missing", "Cannot read ONNX model: " + path);
    try {
        return std::make_unique<Ort::Session>(env, path.toUtf8().constData(), options);
    } catch (const Ort::Exception& error) {
        throw OcrModelError("model_load_failed", "Cannot load " + path + ": " + QString::fromUtf8(error.what()));
    }
}

std::vector<cv::Point2f> Corners(const cv::RotatedRect& rect)
{
    std::vector<cv::Point2f> points(4);
    rect.points(points.data());
    std::sort(points.begin(), points.end(), [](cv::Point2f a, cv::Point2f b) { return a.x < b.x; });
    if (points[0].y > points[1].y) std::swap(points[0], points[1]);
    if (points[2].y > points[3].y) std::swap(points[2], points[3]);
    return {points[0], points[2], points[3], points[1]};
}

std::vector<float> Pixels(const cv::Mat& bgr, bool detection, int paddedWidth)
{
    const float mean[] = {0.485f, 0.456f, 0.406f};
    const float stddev[] = {0.229f, 0.224f, 0.225f};
    std::vector<float> pixels(3 * bgr.rows * paddedWidth, 0.0f);
    for (int y = 0; y < bgr.rows; ++y) {
        const auto* row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x)
            for (int c = 0; c < 3; ++c) {
                float value = row[x][c] / 255.0f;
                pixels[c * bgr.rows * paddedWidth + y * paddedWidth + x] =
                    detection ? (value - mean[c]) / stddev[c] : (value - 0.5f) / 0.5f;
            }
    }
    return pixels;
}
}

OcrEngine::OcrEngine(const QString& detectionModelDir, const QString& recognitionModelDir)
{
    const auto detection = Metadata(detectionModelDir, "PP-OCRv6_small_det");
    const auto recognition = Metadata(recognitionModelDir, "PP-OCRv6_small_rec");
    try {
        for (const auto& metadata : {detection, recognition}) {
            const auto decode = Transform(metadata, "DecodeImage");
            Require(decode["img_mode"].as<std::string>() == "BGR" && !decode["channel_first"].as<bool>(),
                    "Only BGR HWC image decoding is supported");
        }
        const auto normalization = Transform(detection, "NormalizeImage");
        const auto mean = normalization["mean"].as<std::vector<float>>();
        const auto stddev = normalization["std"].as<std::vector<float>>();
        Require(mean == std::vector<float>{0.485f, 0.456f, 0.406f} &&
                stddev == std::vector<float>{0.229f, 0.224f, 0.225f} &&
                normalization["scale"].as<std::string>() == "1./255." &&
                normalization["order"].as<std::string>() == "hwc", "Unsupported detector normalization");
        Require(Transform(detection, "DetResizeForTest").IsNull(), "Unsupported detector resize configuration");
        Transform(detection, "ToCHWImage");
        const auto resize = Transform(recognition, "RecResizeImg");
        const auto imageShape = resize["image_shape"].as<std::vector<int>>();
        Require(imageShape == std::vector<int>{3, 48, 320}, "Unsupported recognition image_shape; expected [3,48,320]");
        _recognitionHeight = imageShape[1];
        _recognitionWidth = imageShape[2];
        const auto detectorPost = detection["PostProcess"], recognizerPost = recognition["PostProcess"];
        Require(detectorPost["name"].as<std::string>() == "DBPostProcess" &&
                recognizerPost["name"].as<std::string>() == "CTCLabelDecode", "Unsupported OCR postprocessing");
        _threshold = detectorPost["thresh"].as<float>();
        _boxThreshold = detectorPost["box_thresh"].as<float>();
        _unclipRatio = detectorPost["unclip_ratio"].as<float>();
        const auto maxCandidates = detectorPost["max_candidates"].as<int>();
        Require(std::isfinite(_threshold) && _threshold > 0 && _threshold < 1 &&
                std::isfinite(_boxThreshold) && _boxThreshold > 0 && _boxThreshold < 1 &&
                std::isfinite(_unclipRatio) && _unclipRatio > 0 && _unclipRatio <= 10 &&
                maxCandidates > 0 && maxCandidates <= 10000, "Invalid DB postprocessing parameters");
        _maxCandidates = size_t(maxCandidates);
        const auto dictionary = recognizerPost["character_dict"];
        Require(dictionary.IsSequence() && dictionary.size() > 0 && dictionary.size() < 100000,
                "Missing or invalid inference.yml character dictionary");
        // PaddleX CTCLabelDecode prepends blank and appends a literal space by default.
        Require(!recognizerPost["use_space_char"] || recognizerPost["use_space_char"].as<bool>(),
                "Unsupported CTC decoder without space character");
        _characters.append("");
        for (const auto& character : dictionary) {
            Require(character.IsScalar(), "Invalid CTC dictionary entry");
            const auto text = QString::fromStdString(character.as<std::string>());
            Require(!text.isEmpty() && !text.contains(QChar::ReplacementCharacter), "Invalid UTF-8 CTC dictionary entry");
            _characters.append(text);
        }
        _characters.append(" ");
    } catch (const YAML::Exception& error) {
        throw OcrModelError("model_incompatible", "Invalid OCR preprocessing/dictionary metadata: " + QString::fromUtf8(error.what()));
    }
    _options.SetIntraOpNumThreads(2);
    _options.SetInterOpNumThreads(1);
    _options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    _detector = LoadSession(_env, _options, detectionModelDir);
    _recognizer = LoadSession(_env, _options, recognitionModelDir);
    ValidateSession(*_detector, true, _recognitionHeight, _characters.size());
    ValidateSession(*_recognizer, false, _recognitionHeight, _characters.size());
    // Check dynamic outputs as well, before the parent sees a ready event.
    try {
        Detect(cv::Mat(64, 64, CV_8UC3, cv::Scalar(255, 255, 255)));
        const cv::Mat line(_recognitionHeight, _recognitionWidth, CV_8UC3, cv::Scalar(255, 255, 255));
        ReadLine(line, {{0, 0}, {float(line.cols - 1), 0}, {float(line.cols - 1), float(line.rows - 1)}, {0, float(line.rows - 1)}});
    } catch (const std::exception& error) {
        throw OcrModelError("model_incompatible", "OCR model validation failed: " + QString::fromUtf8(error.what()));
    }
}

Ort::Value OcrEngine::Run(Ort::Session& session, std::vector<float>& pixels, int height, int width)
{
    const std::array<int64_t, 4> shape{{1, 3, height, width}};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), shape.data(), shape.size());
    Ort::AllocatorWithDefaultOptions allocator;
    const auto inputName = session.GetInputNameAllocated(0, allocator);
    const auto outputName = session.GetOutputNameAllocated(0, allocator);
    const char* inputs[] = {inputName.get()};
    const char* outputs[] = {outputName.get()};
    auto values = session.Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 1);
    if (!values[0].IsTensor() || values[0].GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("unexpected ONNX tensor type");
    return std::move(values[0]);
}

std::vector<std::vector<cv::Point2f>> OcrEngine::Detect(const cv::Mat& image)
{
    // 保留本 helper 的缩放策略：最短边 64、最长边 4000，尺寸为 32 的倍数。
    // 不等同于 PaddleX 当前 small_det 的默认最长边 960。
    double ratio = std::min(image.rows, image.cols) < 64 ? 64.0 / std::min(image.rows, image.cols) : 1.0;
    int height = int(image.rows * ratio), width = int(image.cols * ratio);
    if (std::max(height, width) > 4000) {
        ratio = 4000.0 / std::max(height, width);
        height = int(height * ratio); width = int(width * ratio);
    }
    height = std::max(32, int(std::nearbyint(height / 32.0)) * 32);
    width = std::max(32, int(std::nearbyint(width / 32.0)) * 32);
    cv::Mat resized;
    cv::resize(image, resized, {width, height});
    auto pixels = Pixels(resized, true, width);
    auto output = Run(*_detector, pixels, height, width);
    const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 4 || shape[0] != 1 || shape[1] != 1 || shape[2] < 1 || shape[3] < 1 || shape[2] > 4096 || shape[3] > 4096)
        throw std::runtime_error("unexpected detector output shape");
    cv::Mat probability(int(shape[2]), int(shape[3]), CV_32F, output.GetTensorMutableData<float>());
    cv::Mat bitmap = probability > _threshold;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    std::vector<std::vector<cv::Point2f>> boxes;
    for (size_t index = 0; index < std::min(contours.size(), _maxCandidates); ++index) {
        auto rect = cv::minAreaRect(contours[index]);
        if (std::min(rect.size.width, rect.size.height) < 3) continue;
        auto points = Corners(rect);
        float xmin = points[0].x, xmax = xmin, ymin = points[0].y, ymax = ymin;
        for (auto p : points) { xmin = std::min(xmin, p.x); xmax = std::max(xmax, p.x); ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y); }
        const int left = std::max(0, int(std::floor(xmin))), top = std::max(0, int(std::floor(ymin)));
        const int right = std::min(probability.cols - 1, int(std::ceil(xmax))), bottom = std::min(probability.rows - 1, int(std::ceil(ymax)));
        cv::Mat mask = cv::Mat::zeros(bottom - top + 1, right - left + 1, CV_8U);
        std::vector<cv::Point> local;
        for (auto p : points) local.emplace_back(int(p.x - left), int(p.y - top));
        cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{local}, cv::Scalar(1));
        if (cv::mean(probability(cv::Rect(left, top, mask.cols, mask.rows)), mask)[0] < _boxThreshold) continue;
        // DB 四边形模式先扩张最小面积矩形，再求一次最小面积矩形。
        // 连续圆角偏移的包围矩形，每边增加 2*area*ratio/perimeter。
        // 与 pyclipper 的整数偏移不同，此处保留浮点精度，参见一致性测试。
        const float distance = rect.size.area() * _unclipRatio / (2 * (rect.size.width + rect.size.height));
        rect.size.width += 2 * distance;
        rect.size.height += 2 * distance;
        if (std::min(rect.size.width, rect.size.height) < 5) continue;
        points = Corners(rect);
        for (auto& p : points) {
            p.x = std::max(0.0f, std::min(float(image.cols - 1), float(std::nearbyint(p.x * image.cols / probability.cols))));
            p.y = std::max(0.0f, std::min(float(image.rows - 1), float(std::nearbyint(p.y * image.rows / probability.rows))));
        }
        if (cv::norm(points[0] - points[1]) <= 3 || cv::norm(points[0] - points[3]) <= 3) continue;
        boxes.push_back(points);
    }
    std::sort(boxes.begin(), boxes.end(), [](const auto& a, const auto& b) {
        return a[0].y == b[0].y ? a[0].x < b[0].x : a[0].y < b[0].y;
    });
    // 使用 Paddle 的局部行排序，而非文档版面分析算法。
    for (size_t i = 1; i < boxes.size(); ++i)
        for (size_t j = i; j > 0 && std::abs(boxes[j][0].y - boxes[j - 1][0].y) < 10 && boxes[j][0].x < boxes[j - 1][0].x; --j)
            std::swap(boxes[j], boxes[j - 1]);
    return boxes;
}

QJsonObject OcrEngine::ReadLine(const cv::Mat& image, const std::vector<cv::Point2f>& box)
{
    const int width = std::max(1, int(std::max(cv::norm(box[0] - box[1]), cv::norm(box[2] - box[3]))));
    const int height = std::max(1, int(std::max(cv::norm(box[0] - box[3]), cv::norm(box[1] - box[2]))));
    const std::vector<cv::Point2f> target{{0, 0}, {float(width), 0}, {float(width), float(height)}, {0, float(height)}};
    cv::Mat crop;
    cv::warpPerspective(image, crop, cv::getPerspectiveTransform(box, target), {width, height}, cv::INTER_CUBIC, cv::BORDER_REPLICATE);
    if (height / double(width) >= 1.5) cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
    const int inputWidth = std::min(3200, std::max(_recognitionWidth, int(double(_recognitionHeight) * crop.cols / crop.rows)));
    const int resizedWidth = std::max(1, std::min(inputWidth, int(std::ceil(double(_recognitionHeight) * crop.cols / crop.rows))));
    cv::Mat resized;
    cv::resize(crop, resized, {resizedWidth, _recognitionHeight});
    auto pixels = Pixels(resized, false, inputWidth);
    auto output = Run(*_recognizer, pixels, _recognitionHeight, inputWidth);
    const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 3 || shape[0] != 1 || shape[1] < 1 || shape[2] != _characters.size())
        throw std::runtime_error("recognizer output/dictionary mismatch");
    const auto* data = output.GetTensorData<float>();
    QString text;
    double score = 0;
    int count = 0, previous = -1;
    for (int64_t step = 0; step < shape[1]; ++step) {
        const float* row = data + step * shape[2];
        const int index = int(std::max_element(row, row + shape[2]) - row);
        if (index != 0 && index != previous) {
            text += _characters[index].toString(); score += row[index]; ++count;
        }
        previous = index;
    }
    QJsonArray polygon;
    for (auto p : box) polygon.append(QJsonArray{p.x, p.y});
    return {{"text", text}, {"confidence", count ? score / count : 0.0}, {"polygon", polygon}};
}

QJsonObject OcrEngine::Recognize(const QString& path)
{
    const QFileInfo file(path);
    if (!file.isAbsolute() || !file.isFile() || file.size() > 64 * 1024 * 1024)
        throw std::runtime_error("image must be an absolute file path, at most 64 MiB");
    QElapsedTimer timer;
    timer.start();
    QImageReader reader(path);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096)
        throw std::runtime_error("invalid image or image exceeds 4096 pixels per side");
    if (reader.imageCount() > 1) throw std::runtime_error("only single-frame images are supported");
    auto rgb = reader.read().convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull() || rgb.size() != size) throw std::runtime_error("failed to decode image");
    cv::Mat image;
    cv::cvtColor(cv::Mat(rgb.height(), rgb.width(), CV_8UC3, rgb.bits(), rgb.bytesPerLine()), image, cv::COLOR_RGB2BGR);
    QJsonArray lines;
    for (const auto& box : Detect(image)) lines.append(ReadLine(image, box));
    return {{"width", image.cols}, {"height", image.rows}, {"lines", lines}, {"elapsed_s", timer.nsecsElapsed() / 1e9}};
}
