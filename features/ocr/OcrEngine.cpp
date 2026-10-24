// OCR preprocessing, DB quad and CTC pipeline follow PaddleOCR (PaddlePaddle Authors).
// See LICENSE-PaddleOCR and NOTICE for attribution and the float-unclip difference.
#include "OcrEngine.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <opencv2/imgproc.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry/2d.hpp>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
QJsonDocument JsonFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("missing OCR bundle file");
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) throw std::runtime_error("invalid OCR bundle JSON");
    return doc;
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

OcrEngine::OcrEngine(const QString& models)
{
    const QDir directory(models);
    const auto manifest = JsonFile(directory.filePath("manifest.json")).object();
    if (manifest.value("profile").toString() != "ppocrv6-small-medium-v1")
        throw std::runtime_error("unsupported OCR model profile");
    const auto hashes = manifest.value("sha256").toObject();
    for (const auto* name : {"detection.onnx", "recognition.onnx", "dictionary.json"}) {
        QFile file(directory.filePath(name));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) ||
            hash.result().toHex() != hashes.value(name).toString().toLatin1())
            throw std::runtime_error("OCR bundle checksum mismatch or missing file");
    }
    _characters = JsonFile(directory.filePath("dictionary.json")).array();
    if (_characters.isEmpty() || _characters[0].toString() != "") throw std::runtime_error("invalid CTC dictionary");
    for (const auto& value : _characters)
        if (!value.isString()) throw std::runtime_error("invalid CTC dictionary entry");
    _options.SetIntraOpNumThreads(2);
    _options.SetInterOpNumThreads(1);
    _options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    _detector.reset(new Ort::Session(_env, directory.filePath("detection.onnx").toUtf8().constData(), _options));
    _recognizer.reset(new Ort::Session(_env, directory.filePath("recognition.onnx").toUtf8().constData(), _options));
    for (auto* session : {_detector.get(), _recognizer.get()})
        if (session->GetInputCount() != 1 || session->GetOutputCount() != 1)
            throw std::runtime_error("unexpected ONNX input/output count");
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
    // Match the PaddleOCR general pipeline: min side 64, max side 4000, multiples of 32.
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
    cv::Mat bitmap = probability > 0.3f;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    std::vector<std::vector<cv::Point2f>> boxes;
    for (size_t index = 0; index < std::min<size_t>(contours.size(), 3000); ++index) {
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
        if (cv::mean(probability(cv::Rect(left, top, mask.cols, mask.rows)), mask)[0] < 0.6) continue;
        // DB quad mode expands a minimum-area rectangle, then takes its minimum-area rectangle again.
        // Its continuous round-offset bounding rectangle grows by 2*area*ratio/perimeter per side.
        // Unlike pyclipper's integer offset, this retains float precision (see parity tests).
        const float distance = rect.size.area() * 1.5f / (2 * (rect.size.width + rect.size.height));
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
    // Paddle's local row ordering, not a document-layout algorithm.
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
    const int inputWidth = std::min(3200, std::max(320, int(48.0 * crop.cols / crop.rows)));
    const int resizedWidth = std::min(inputWidth, int(std::ceil(48.0 * crop.cols / crop.rows)));
    cv::Mat resized;
    cv::resize(crop, resized, {resizedWidth, 48});
    auto pixels = Pixels(resized, false, inputWidth);
    auto output = Run(*_recognizer, pixels, 48, inputWidth);
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
