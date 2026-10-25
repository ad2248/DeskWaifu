#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <iostream>

void Emit(QJsonObject object)
{
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto mode = qEnvironmentVariable("DESKWAIFU_OCR_TEST_MODE");
    if (mode == "no-ready") { QThread::sleep(10); return 0; }
    if (mode == "load-error") { Emit({{"event", "error"}, {"protocol", 1}, {"error", "model checksum mismatch"}}); return 1; }
    Emit({{"event", "ready"}, {"protocol", mode == "version" ? 2 : 1}});
    if (mode == "crash") return 3;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (mode == "hang") { QThread::sleep(10); return 0; }
        if (mode == "oversized") { std::cout << std::string(4 * 1024 * 1024 + 1, 'x') << std::flush; QThread::sleep(10); return 0; }
        if (mode == "garbage") { std::cout << "not-json\n" << std::flush; continue; }
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto id = mode == "bad-id" ? QJsonValue("wrong-id") : request.value("id");
        if (mode == "error") { Emit({{"protocol", 1}, {"id", id}, {"ok", false}, {"error", "test request error"}}); continue; }
        QJsonObject result{{"width", 1}, {"height", 1}, {"lines", QJsonArray{}}, {"helper_pid", QCoreApplication::applicationPid()}};
        if (mode == "text") result.insert("lines", QJsonArray{QJsonObject{
            {"text", "synthetic clipboard text"}, {"confidence", 0.9},
            {"polygon", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 0}, QJsonArray{1, 1}, QJsonArray{0, 1}}}}});
        if (mode == "bad-result") result.insert("lines", QJsonArray{QJsonObject{{"text", "bad"}}});
        Emit({{"protocol", 1}, {"id", id}, {"ok", true}, {"result", result}});
    }
}
