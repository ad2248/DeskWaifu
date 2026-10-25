#include "OcrEngine.hpp"
#include <QCoreApplication>
#include <QDir>
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
    QString models = QDir(QCoreApplication::applicationDirPath()).filePath(OCR_MODELS_RELATIVE);
    if (arguments.size() >= 2 && arguments[0] == "--models") {
        models = arguments[1]; arguments.remove(0, 2);
    }
    if (arguments != QStringList{"--serve"}) {
        std::cerr << "Usage: deskwaifu-ocr [--models DIRECTORY] --serve\n";
        return 2;
    }
    try {
        OcrEngine engine(models);
        Emit({{"event", "ready"}, {"backend", "onnxruntime-cpu"},
            {"runtime_version", QString::fromUtf8(OrtGetApiBase()->GetVersionString())},
            {"profile", "ppocrv6-small-medium-v1"}});
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
    } catch (const std::exception& error) {
        Emit({{"event", "error"}, {"error", QString::fromUtf8(error.what())}});
        return 1;
    }
    return 0;
}
