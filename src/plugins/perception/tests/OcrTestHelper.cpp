#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QFile>
#include <iostream>

void Emit(QJsonObject object)
{
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto mode = qEnvironmentVariable("DESKWAIFU_OCR_TEST_MODE");
    if (mode == "arguments") {
        const auto arguments = app.arguments();
        if (arguments.mid(1) != QStringList{"--detection-model-dir", "/tmp/检测 模型", "--recognition-model-dir", "/tmp/识别 模型", "--serve"}) {
            Emit({{"event", "error"}, {"protocol", 1}, {"error", "incorrect model arguments"}}); return 1;
        }
    }
    if (mode == "no-ready") { QThread::sleep(10); return 0; }
    if (mode == "load-error") { Emit({{"event", "error"}, {"protocol", 1}, {"error", "model checksum mismatch"}}); return 1; }
    Emit({{"event", "ready"}, {"protocol", mode == "version" ? 2 : 1}});
    if (mode == "crash") return 3;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto modePath = qEnvironmentVariable("DESKWAIFU_OCR_TEST_MODE_FILE");
        if (!modePath.isEmpty()) {
            QFile file(modePath);
            if (!file.open(QIODevice::ReadOnly)) return 4;
            mode = QString::fromUtf8(file.readAll()).trimmed();
        }
        if (mode == "hang") { QThread::sleep(10); return 0; }
        if (mode == "crash-request") return 3;
        if (mode == "oversized") { std::cout << std::string(4 * 1024 * 1024 + 1, 'x') << std::flush; QThread::sleep(10); return 0; }
        if (mode == "garbage") { std::cout << "not-json\n" << std::flush; continue; }
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto id = mode == "bad-id" ? QJsonValue("wrong-id") : request.value("id");
        if (mode == "delayed-reply") QThread::msleep(50);
        if (mode == "error") { Emit({{"protocol", 1}, {"id", id}, {"ok", false}, {"error", "test request error"}}); continue; }
        QJsonObject result{{"width", 1}, {"height", 1}, {"lines", QJsonArray{}}, {"helper_pid", QCoreApplication::applicationPid()}};
        if (mode == "text") result.insert("lines", QJsonArray{QJsonObject{
            {"text", "synthetic clipboard text"}, {"confidence", 0.9},
            {"polygon", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 0}, QJsonArray{1, 1}, QJsonArray{0, 1}}}}});
        if (mode == "bad-result") result.insert("lines", QJsonArray{QJsonObject{{"text", "bad"}}});
        Emit({{"protocol", 1}, {"id", id}, {"ok", true}, {"result", result}});
    }
}
