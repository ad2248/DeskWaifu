#include "LoadedBackendPlugins.hpp"
#include "contracts/ImageFile.hpp"
#include "TestImageFile.hpp"
#include <QGuiApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "public plugin protocol completion timed out");
}
void Send(QLocalSocket& socket, QJsonObject request)
{ socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); socket.flush(); }
QJsonObject Receive(QLocalSocket& socket)
{
    Wait([&] { return socket.canReadLine(); });
    return QJsonDocument::fromJson(socket.readLine()).object();
}
QJsonObject Request(QLocalSocket& socket, QJsonObject request) { Send(socket, request); return Receive(socket); }
}
int main(int argc, char** argv)
try {
    QGuiApplication app(argc, argv);
    Check(argc == 5, "expected IPC, perception, reading modules and OCR fixture");
    QTemporaryDir directory; Check(directory.isValid(), "private runtime");
    qputenv("XDG_RUNTIME_DIR", directory.path().toUtf8());
    qputenv("DESKWAIFU_OCR_HELPER", app.arguments()[4].toUtf8());
    qputenv("DESKWAIFU_OCR_TEST_MODE", "text");
    const auto modePath = directory.path() + "/ocr-mode";
    qputenv("DESKWAIFU_OCR_TEST_MODE_FILE", modePath.toUtf8());
    auto mode = [&](const QByteArray& value) {
        QFile file(modePath); Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "OCR mode fixture");
        Check(file.write(value) == value.size(), "write OCR mode fixture");
    };
    mode("text");
    QImage image(4, 4, QImage::Format_ARGB32); image.fill(Qt::blue);
    QGuiApplication::clipboard()->setImage(image);
    ProcessorEventLoop loop;
    RequestId pendingModel = 0;
    int modelIndex = 0, selections = 0, spoken = 0, quitRequests = 0;
    std::unique_ptr<EventEndpoint> model;
    model = loop.CreateEndpoint("public-model-fixture", [&](const EventDelivery& delivery) {
        if (delivery.kind != EventDelivery::Kind::Request || !delivery.payload) return;
        const auto input = QJsonDocument::fromJson(delivery.payload->data.value("json").toString().toUtf8()).object();
        const auto command = input.value("command").toString();
        if (command == "set_model") { ++selections; pendingModel = delivery.requestId; return; }
        if (command == "quit") ++quitRequests;
        QJsonObject reply{{"ok", true}, {"model_index", modelIndex}, {"model", modelIndex ? "two" : "one"}};
        model->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
            QVariantMap{{"json", QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact))}}));
    });
    for (const auto* command : {"get_status", "get_current_model", "set_model", "quit"})
        Check(model->Provide("command." + QString::fromLatin1(command) + "/v1"), "provide public model command");
    std::unique_ptr<EventEndpoint> speech;
    speech = loop.CreateEndpoint("public-speech-fixture", [&](const EventDelivery& delivery) {
        if (delivery.kind != EventDelivery::Kind::Request || !delivery.payload) return;
        Check(delivery.payload->data.value("text") == "synthetic clipboard text", "reading must use recognized text");
        ++spoken;
        speech->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("speech.spoken"));
    });
    Check(speech->Provide("speech.speak/v1"), "provide speech protocol fixture");
    LoadedBackendPlugins plugins(loop, {app.arguments()[1], app.arguments()[2], app.arguments()[3]});
    QLocalSocket first, second;
    const auto path = directory.path() + "/deskwaifu.sock";
    first.connectToServer(path); second.connectToServer(path);
    Check(first.waitForConnected(1000) && second.waitForConnected(1000), "private IPC connections");
    Check(Request(second, {{"command", "get_status"}}).value("model_index") == 0, "public metadata query");
    Send(first, {{"command", "set_model"}, {"index", 1}});
    Send(first, {{"command", "get_current_model"}});
    Wait([&] { return pendingModel != 0; });
    Check(::shutdown(static_cast<int>(first.socketDescriptor()), SHUT_WR) == 0, "half-close write end");
    Check(!first.canReadLine(), "model result must not be fabricated on enqueue");
    Check(Request(second, {{"command", "get_status"}}).value("model_index") == 0, "other connection progresses during slow model operation");
    Check(Request(second, {{"command", "get_ocr_status"}}).value("ok").toBool(), "OCR API via public capability");
    modelIndex = 1;
    Check(model->Respond(pendingModel, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
        QVariantMap{{"json", QStringLiteral("{\"ok\":true}")}})), "complete asynchronous model provider");
    pendingModel = 0;
    Check(Receive(first).value("ok").toBool(), "actual asynchronous model result");
    Check(Receive(first).value("model") == "two", "per-connection pipeline and state ordering after half-close");

    const auto externalFile = MakeTestImageFile(image);
    Check(bool(externalFile), "private external OCR image");
    Wait([&] { return Request(second, {{"command", "get_ocr_status"}}).value("available").toBool(); });
    const auto accepted = Request(second, {{"command", "ocr_recognize"}, {"image", externalFile->Path()}});
    const auto job = accepted.value("job_id").toString();
    Check(accepted.value("ok").toBool() && !job.isEmpty(), "external OCR accepted with original job id");
    QJsonObject recognized;
    Wait([&] {
        recognized = Request(second, {{"command", "get_ocr_result"}, {"job_id", job}});
        return recognized.value("state") != "pending";
    });
    Check(recognized.value("response").toObject().value("ok").toBool(), "external OCR actual completion retained");
    mode("hang");
    const auto hanging = Request(second, {{"command", "ocr_recognize"}, {"image", externalFile->Path()}});
    Check(hanging.value("ok").toBool(), "external cancellable OCR accepted");
    Check(Request(second, {{"command", "cancel_ocr"}, {"job_id", hanging.value("job_id")}}).value("ok").toBool(), "external OCR cancellation protocol");
    Wait([&] { return !Request(second, {{"command", "get_ocr_status"}}).value("running").toBool(); });
    mode("text");
    Check(Request(second, {{"command", "reading_clip"}}).value("queued").toBool(), "reading event queued");
    Wait([&] { return spoken == 1; });
    Check(Request(second, {{"command", "get_reading_clip_status"}}).value("state") == "idle", "complete real image/OCR/reading module chain");
    second.write("{\"command\":\"quit\",\"command\":\"get_status\"}\n"); second.flush();
    Check(!Receive(second).value("ok").toBool(), "invalid duplicate-key quit must not close the transport");
    Check(quitRequests == 0, "invalid duplicate-key quit must not reach a real quit capability");
    Check(Request(second, {{"command", "get_status"}}).value("ok").toBool(), "transport remains live after invalid quit");
    Send(second, {{"command", "set_model"}, {"index", 0}});
    Wait([&] { return pendingModel != 0; });
    Send(second, {{"command", "set_model"}, {"index", 1}});
    const int beforeClose = selections;
    plugins.Stop();
    Check(!model->Respond(pendingModel, std::make_shared<ProcessorEvent>("command.result")), "late result cannot target unloaded transport");
    for (int i = 0; i < 50; ++i) QCoreApplication::processEvents();
    Check(selections == beforeClose, "queued model actions cannot run after transport unload");
    std::cout << "Public plugin IPC/OCR/reading routing, half-close and unload ownership passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
