#include "plugin/PluginManager.hpp"
#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 2000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "perception command completion timed out");
}
void Copy(const QString& source, const QString& root, const QString& name)
{
    Check(QDir().mkpath(root + '/' + name), "create plugin directory");
    Check(QFile::copy(source, root + '/' + name + '/' + name + ".so"), "copy module");
}
QJsonObject Json(const RequestResult& result)
{
    Check(result.status == RequestStatus::Replied && bool(result.payload), "command must reply");
    return QJsonDocument::fromJson(result.payload->data.value("json").toString().toUtf8()).object();
}
}

int main(int argc, char** argv)
try {
    QGuiApplication app(argc, argv);
    Check(argc == 3, "expected early requester and real perception module");
    QTemporaryDir directory; Check(directory.isValid(), "temporary installation");
    qputenv("DESKWAIFU_OCR_HELPER", (directory.path() + "/no-helper").toUtf8());
    const auto root = directory.path() + "/plugins";
    Copy(argv[1], root, "a");
    Copy(argv[2], root, "perception");
    QString error;
    Check(WriteJsonObject(directory.path() + "/plugin/perception/config.json",
          {{"ocr_detection_model_dir", directory.path() + "/检测 模型"}}, &error), "write perception model configuration");
    ProcessorEventLoop loop;
    PluginManager plugins(loop, root, directory.path());
    Check(plugins.Start(&error), qPrintable(error));
    QJsonObject early;
    Check(ReadJsonObject(directory.path() + "/plugin/a/early.json", early, &error), "read early response");
    Check(early.value("completed").toBool() && early.value("status") == "replied" &&
          early.value("response").toObject() == QJsonObject{{"ok", false}, {"error", "perception is not ready"}},
          "request before perception Start gets an immediate readiness error");

    auto client = loop.CreateEndpoint("client", [](const EventDelivery&) {});
    std::map<RequestId, RequestResult> results;
    std::map<RequestId, int> completions;
    const auto request = [&](const QString& command, int timeout = 1000) {
        auto payload = std::make_shared<ProcessorEvent>("command.invoke", nullptr, nullptr,
            QVariantMap{{"json", QString::fromUtf8(QJsonDocument(QJsonObject{{"command", command}, {"job_id", "missing"}}).toJson(QJsonDocument::Compact))}});
        const auto id = client->Request("command." + command + "/v1", payload, [&](const RequestResult& result) {
            ++completions[result.requestId]; results[result.requestId] = result;
        }, timeout);
        Check(id != 0, "enqueue command");
        return id;
    };
    const auto status = [&] {
        const auto id = request("get_ocr_status");
        Wait([&] { return results.count(id) != 0; });
        const auto reply = Json(results.at(id));
        Check(reply.value("ok").toBool() && !reply.value("available").toBool() &&
              reply.value("state") == "idle" && reply.value("job_id") == "" &&
              reply.value("running") == false, "status is paired with its own request");
        Check(reply.value("availability_state") == "unavailable" && reply.value("error_code") == "helper_missing",
              "missing extension disables only OCR and reports its reason");
        Check(reply.value("detection_model_dir") == directory.path() + "/检测 模型" &&
              reply.value("recognition_model_dir") == "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec",
              "perception reads custom model path and preserves independent default");
    };
    for (int i = 0; i < 3; ++i) status();

    // Deliver to the real command endpoint, then retire the RPC before its
    // queued legacy OCR response. The following replies must not shift by one.
    const auto cancelled = request("get_ocr_status");
    QCoreApplication::sendPostedEvents(&loop);
    Check(client->Cancel(cancelled), "cancel delivered command");
    const auto missing = request("get_ocr_result");
    Wait([&] { return results.count(cancelled) && results.count(missing); });
    Check(results.at(cancelled).status == RequestStatus::Cancelled, "cancelled request completes once");
    Check(Json(results.at(missing)).value("error") == "unknown OCR job; only the most recent job is retained",
          "cancelled status cannot consume following result response");
    status();

    const auto timedOut = request("get_ocr_status", 1);
    QCoreApplication::sendPostedEvents(&loop);
    QThread::msleep(5);
    const auto cancelJob = request("cancel_ocr");
    Wait([&] { return results.count(timedOut) && results.count(cancelJob); });
    Check(results.at(timedOut).status == RequestStatus::TimedOut, "delivered command can time out");
    Check(Json(results.at(cancelJob)).value("error") == "no matching pending OCR job",
          "timed-out status cannot consume following cancel response");
    for (int i = 0; i < 3; ++i) status();
    Check(plugins.Stop(&error), qPrintable(error));
    for (int i = 0; i < 50; ++i) QCoreApplication::processEvents();
    for (const auto& entry : completions) Check(entry.second == 1, "every command has exactly one terminal completion");
    std::cout << "Real perception loading: early readiness, repeated status, cancellation and timeout pairing passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
