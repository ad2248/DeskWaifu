#include "OcrEngine.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>

namespace {
void Emit(QJsonObject object)
{
    object.insert("protocol", 1);
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto arguments = app.arguments();
    arguments.removeFirst();
    QString detectionModelDir = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det";
    QString recognitionModelDir = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec";
    try {
        bool serve = false, detectionSet = false, recognitionSet = false;
        while (!arguments.isEmpty()) {
            const auto option = arguments.takeFirst();
            if (option == "--serve" && !serve) serve = true;
            else if ((option == "--detection-model-dir" && !detectionSet) ||
                     (option == "--recognition-model-dir" && !recognitionSet)) {
                if (arguments.isEmpty() || arguments.first().isEmpty() || arguments.first().startsWith("--"))
                    throw OcrModelError("invalid_arguments", "Missing directory for " + option);
                if (option == "--detection-model-dir") { detectionModelDir = arguments.takeFirst(); detectionSet = true; }
                else { recognitionModelDir = arguments.takeFirst(); recognitionSet = true; }
            } else throw OcrModelError("invalid_arguments", "Unknown or duplicate option: " + option);
        }
        if (!serve) throw OcrModelError("invalid_arguments", "Expected --serve");
        OcrEngine engine(detectionModelDir, recognitionModelDir);
        Emit({{"event", "ready"}, {"backend", "onnxruntime-cpu"},
            {"runtime_version", QString::fromUtf8(OrtGetApiBase()->GetVersionString())},
            {"detection_model", "PP-OCRv6_small_det"}, {"recognition_model", "PP-OCRv6_small_rec"}});
        std::string line;
        while (std::cin.good()) {
            line.clear();
            bool overflow = false;
            char character;
            while (std::cin.get(character) && character != '\n') {
                if (line.size() < 64 * 1024) line += character;
                else overflow = true;
            }
            if (line.empty() && !std::cin.good()) break;
            QJsonValue id;
            try {
                if (overflow) throw std::runtime_error("request exceeds 64 KiB");
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
                const auto request = document.object();
                id = request.value("id");
                if (error.error != QJsonParseError::NoError || !document.isObject() ||
                    request.value("protocol") != QJsonValue(1) || !id.isString() || id.toString().isEmpty() ||
                    id.toString().size() > 128 || !request.value("image").isString())
                    throw std::runtime_error("expected protocol=1, string id and image path");
                Emit({{"id", id}, {"ok", true}, {"result", engine.Recognize(request.value("image").toString())}});
            } catch (const std::exception& error) {
                Emit({{"id", id}, {"ok", false}, {"error", QString::fromUtf8(error.what())}});
            }
        }
    } catch (const OcrModelError& error) {
        Emit({{"event", "error"}, {"code", error.code}, {"error", QString::fromUtf8(error.what())}});
        return QString::fromLatin1(error.code) == "invalid_arguments" ? 2 : 1;
    } catch (const std::exception& error) {
        Emit({{"event", "error"}, {"code", "model_load_failed"}, {"error", QString::fromUtf8(error.what())}});
        return 1;
    }
    return 0;
}
