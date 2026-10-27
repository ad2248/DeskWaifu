#include "plugin/PluginManager.hpp"
#include "contracts/ImageFile.hpp"
#include "TestImageFile.hpp"
#include <QGuiApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "reading plugin completion timed out");
}
QJsonObject Command(EventEndpoint& client, const QString& command)
{
    bool done = false; QJsonObject reply;
    auto payload = std::make_shared<ProcessorEvent>("command.invoke", nullptr, nullptr,
        QVariantMap{{"json", QString::fromUtf8(QJsonDocument(QJsonObject{{"command", command}}).toJson(QJsonDocument::Compact))}});
    Check(client.Request("command." + command + "/v1", payload, [&](const RequestResult& result) {
        Check(result.status == RequestStatus::Replied && bool(result.payload), "command provider must respond");
        reply = QJsonDocument::fromJson(result.payload->data.value("json").toString().toUtf8()).object();
        done = true;
    }) != 0, "enqueue command");
    Wait([&] { return done; }); return reply;
}
}

int main(int argc, char** argv)
try {
    QGuiApplication app(argc, argv);
    Check(argc == 4, "expected reading module, perception module and OCR fixture");
    qputenv("DESKWAIFU_OCR_HELPER", app.arguments()[3].toUtf8());
    qputenv("DESKWAIFU_OCR_TEST_MODE", "text");
    QImage image(4, 4, QImage::Format_ARGB32); image.fill(Qt::blue);
    QGuiApplication::clipboard()->setImage(image);
    for (const auto& scenario : {QString("complete"), QString("cancel"), QString("no-speech"), QString("no-ocr"), QString("stop")}) {
        QTemporaryDir directory; Check(directory.isValid(), "private plugin installation");
        const auto root = directory.path() + "/plugins";
        Check(QDir().mkpath(root + "/reading_clip"), "plugin directory");
        Check(QFile::copy(app.arguments()[1], root + "/reading_clip/reading_clip.so"), "install reading module");
        if (scenario != "no-ocr") {
            Check(QDir().mkpath(root + "/perception"), "perception plugin directory");
            Check(QFile::copy(app.arguments()[2], root + "/perception/perception.so"), "install perception module");
        }
        ProcessorEventLoop loop;
        std::unique_ptr<EventEndpoint> images;
        images = loop.CreateEndpoint("test-image-only", [&](const EventDelivery& delivery) {
            if (delivery.kind == EventDelivery::Kind::Request)
                images->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("image.ready", nullptr, nullptr,
                    QVariantMap{{"image", QVariant::fromValue(MakeTestImageFile(image))}}));
        });
        if (scenario == "no-ocr") Check(images->Provide("image.read/v1"), "provide image without OCR");
        int spoken = 0, cancelled = 0; RequestId pending = 0;
        auto speech = loop.CreateEndpoint("test-speech", [&](const EventDelivery& delivery) {
            if (delivery.kind == EventDelivery::Kind::Cancellation) { ++cancelled; return; }
            Check(delivery.payload && delivery.payload->data.value("text") == "synthetic clipboard text", "speech receives OCR output");
            ++spoken; pending = delivery.requestId;
        });
        if (scenario != "no-speech") Check(speech->Provide("speech.speak/v1"), "provide speech");
        QStringList states; QString error;
        auto observer = loop.CreateEndpoint("observer", [&](const EventDelivery& delivery) {
            states.append(delivery.payload->data.value("state").toString());
            error = delivery.payload->data.value("error").toString();
        });
        observer->Subscribe("reading.state/v1");
        auto client = loop.CreateEndpoint("commands", [](const EventDelivery&) {});
        PluginManager plugins(loop, root, directory.path());
        QString diagnostic; Check(plugins.Start(&diagnostic), qPrintable(diagnostic));
        Check(Command(*client, "reading_clip").value("queued").toBool(), "reading accepted");
        if (scenario == "no-speech" || scenario == "no-ocr") {
            Wait([&] { return states.endsWith("idle"); });
            Check(!error.isEmpty() && spoken == 0, "missing capability must finish with an error, not hang");
        } else {
            Wait([&] { return pending != 0; });
            if (scenario == "complete") {
                Check(speech->Respond(pending, std::make_shared<ProcessorEvent>("speech.completed")), "complete speech");
                Wait([&] { return states.endsWith("idle"); });
                Check(error.isEmpty() && spoken == 1, "reading success");
                Check(states == QStringList{"reading_image", "recognizing", "speaking", "idle"}, "ordered state notifications");
            } else if (scenario == "cancel") {
                Command(*client, "reading_clip");
                Wait([&] { return states.endsWith("idle") && cancelled == 1; });
                Check(error == "reading cancelled", "cancelled pipeline preserves reason");
                Check(!speech->Respond(pending, std::make_shared<ProcessorEvent>("speech.completed")), "late result cannot resume pipeline");
            } else {
                plugins.Stop();
                Wait([&] { return cancelled == 1; });
                Check(!speech->Respond(pending, std::make_shared<ProcessorEvent>("speech.completed")), "stopped plugin cannot receive late result");
            }
        }
        if (scenario != "stop") {
            const auto status = Command(*client, "get_reading_clip_status");
            Check(status.value("state") == "idle", "query command is plugin-owned");
        }
        plugins.Stop();
    }
    std::cout << "Dynamic reading plugin: success, cancellation, missing providers and shutdown passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
