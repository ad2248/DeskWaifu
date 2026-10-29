#include "LoadedBackendPlugins.hpp"
#include "TestImageFile.hpp"
#include "EventLoopFence.hpp"
#include "processor/Events.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done, const char* description)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), description);
}
}
int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 4, "expected scenario, reading plugin and speech fixture");
    const auto scenario = app.arguments()[1];
    ProcessorEventLoop loop;
    LoadedBackendPlugins plugins(loop, {app.arguments()[2], app.arguments()[3]});
    RequestId imageRequest = 0, ocrRequest = 0;
    auto images = loop.CreateEndpoint("contract-images", [&](const EventDelivery& delivery) {
        if (delivery.kind == EventDelivery::Kind::Request) imageRequest = delivery.requestId;
    });
    auto ocr = loop.CreateEndpoint("contract-ocr", [&](const EventDelivery& delivery) {
        if (delivery.kind == EventDelivery::Kind::Request) ocrRequest = delivery.requestId;
    });
    Check(images->Provide("image.read/v1") && ocr->Provide("ocr.recognize/v1"), "provide public reading capabilities");
    QStringList states;
    auto observer = loop.CreateEndpoint("reading-state-observer", [&](const EventDelivery& delivery) {
        states.append(delivery.payload->data.value("state").toString());
    });
    observer->Subscribe("reading.state/v1");
    auto client = loop.CreateEndpoint("reading-contract-client", [](const EventDelivery&) {});
    auto request = [&](const QString& capability, QVariantMap data = QVariantMap{}) {
        bool done = false; QVariantMap response;
        Check(client->Request(capability, std::make_shared<ProcessorEvent>("test.request", nullptr, nullptr, data),
            [&](const RequestResult& result) {
                Check(result.status == RequestStatus::Replied && bool(result.payload), "public fixture response");
                response = result.payload->data; done = true;
            }) != 0, "enqueue public fixture request");
        Wait([&] { return done; }, "fixture request completed"); return response;
    };
    auto command = [&](const QString& name) {
        return QJsonDocument::fromJson(request("command." + name + "/v1").value("json").toString().toUtf8()).object();
    };
    auto speech = [&](const QString& action) { return request("test.speech.control/v1", {{"action", action}}); };
    Check(command("reading_clip").value("queued").toBool(), "start reading");
    Wait([&] { return imageRequest != 0; }, "reading requests image");
    QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::blue);
    auto file = MakeTestImageFile(image); Check(bool(file), "create private image"); const auto path = file->Path();
    Check(images->Respond(imageRequest, std::make_shared<ProcessorEvent>(ProcessorEvents::ImageReady, nullptr, nullptr,
        QVariantMap{{"image", QVariant::fromValue(file)}})), "image completion"); file.clear();
    Wait([&] { return ocrRequest != 0; }, "image forwarded to OCR");
    Check(ocr->Respond(ocrRequest, std::make_shared<ProcessorEvent>(ProcessorEvents::Recognized, nullptr, nullptr,
        QVariantMap{{"text", "recognized"}})), "OCR completion");
    Wait([&] { return speech("snapshot").value("starts").toInt() == 1; }, "real speech adapter starts recognized text");
    Check(!QFileInfo::exists(path), "reading releases image before speech");
    const auto id = speech("snapshot").value("id");
    if (scenario == "reading-cancel") {
        command("reading_clip");
        Wait([&] { return command("get_reading_clip_status").value("state") == "idle"; }, "cancelled reading reaches Idle");
        Check(command("get_reading_clip_status").value("error") == "reading cancelled", "reading preserves cancellation reason");
        Wait([&] { return speech("snapshot").value("stops").toInt() == 1; }, "speech cancellation drains");
        const auto snapshot = speech("snapshot");
        Check(snapshot.value("cancels").toInt() == 1 && snapshot.value("stops").toInt() == 1 &&
            snapshot.value("cancel_id") == id && snapshot.value("stop_id") == id, "cancel releases source and audio exactly once");
    } else {
        speech("finish");
        Wait([&] { return command("get_reading_clip_status").value("state") == "idle"; }, "completed reading reaches Idle");
        Check(command("get_reading_clip_status").value("error").toString().isEmpty(), "normal reading succeeds");
    }
    speech("late"); Check(AwaitProcessorEvents(loop), "late result FIFO fence");
    const auto snapshot = speech("snapshot");
    Check(snapshot.value("starts").toInt() == 1 && snapshot.value("pcm_empty").toBool(), "late reading results cannot restart or append audio");
    QStringList expected{"reading_image", "recognizing", "speaking"};
    if (scenario == "reading-cancel") expected.append("cancelling");
    expected.append("idle");
    Check(states == expected, "reading state sequence and terminal count must match exactly");
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
