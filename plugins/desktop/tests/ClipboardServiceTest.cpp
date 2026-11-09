#include "clipboard/ClipboardService.hpp"
#include "contracts/ClipboardEvents.hpp"
#include "event/ProcessorEventLoop.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <cerrno>
#include <signal.h>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done, const QString& context,
          const std::function<QString()>& diagnostic = {})
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 6000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    if (!done()) throw std::runtime_error(("clipboard wait timed out: " + context +
        (diagnostic ? "; " + diagnostic() : QString())).toStdString());
}
pid_t Pid(const QString& path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return 0; return file.readAll().toLongLong(); }
bool Dead(pid_t pid)
{
    if (kill(pid, 0) == -1 && errno == ESRCH) return true;
    // A killed orphan may briefly remain a zombie under the container's PID 1.
    QFile status(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!status.open(QIODevice::ReadOnly)) return true;
    const auto data = status.readAll();
    return data.mid(data.lastIndexOf(')') + 2, 1) == "Z";
}
}
int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "fixture path required");
    QTemporaryDir temp; Check(temp.isValid(), "temporary directory");
    ProcessorEventLoop loop;
    ClipboardService* bridge = nullptr;
    auto provider = loop.CreateEndpoint("clipboard-test/provider", [&](const EventDelivery& delivery) { bridge->Deliver(delivery); });
    Check(provider->Provide(ClipboardEvents::WriteTextCapability) && provider->Provide(ClipboardEvents::CommandCapability), "capabilities");
    auto service = std::make_unique<ClipboardService>(*provider, QString::fromLocal8Bit(argv[1])); bridge = service.get();
    auto client = loop.CreateEndpoint("clipboard-test/client", [](const EventDelivery&) {});
    std::map<RequestId, RequestResult> results;
    auto request = [&](QVariantMap data, bool external = false, QString type = {}) {
        return client->Request(external ? ClipboardEvents::CommandCapability : ClipboardEvents::WriteTextCapability,
            std::make_shared<ProcessorEvent>(type.isEmpty() ? (external ? "command.invoke" : ClipboardEvents::WriteText) : type,
                nullptr, nullptr, data), [&](const RequestResult& result) {
                    Check(!results.count(result.requestId), "only one terminal result"); results.emplace(result.requestId, result);
                }, 10000);
    };
    auto result = [&](RequestId id) {
        Wait([&] { return results.count(id); }, QStringLiteral("response request=%1").arg(id));
        Check(results.at(id).status == RequestStatus::Replied, "domain response");
        const auto payload = results.at(id).payload;
        if (payload->type == "command.result") return QJsonDocument::fromJson(payload->data.value("json").toString().toUtf8()).object().toVariantMap();
        Check(payload->type == ClipboardEvents::WriteResult, "internal result type");
        return payload->data;
    };
    auto failure = [&](QVariantMap data, const QString& code, bool external = false, QString type = {}) {
        const auto reply = result(request(data, external, type));
        Check(reply.value("ok").metaType().id() == QMetaType::Bool && !reply.value("ok").toBool(), "typed failure");
        Check(reply.value("error_code").toString() == code, "expected error code");
    };
    auto ready = [&](const QString& context) {
        // A domain error can arrive before QProcess::finished clears the single-flight
        // slot. Even ESRCH is not a Qt reaping barrier: explicitly observe readiness.
        RequestId probe = 0;
        Wait([&] {
            if (!probe) { probe = request({{"text", "readiness-probe"}}); return false; }
            if (!results.count(probe)) return false;
            const auto& response = results.at(probe);
            Check(response.status == RequestStatus::Replied && response.payload, "readiness probe response");
            const auto& data = response.payload->data;
            if (data.value("ok").toBool()) return true;
            Check(data.value("error_code") == "busy", "readiness probe must succeed or report pending reap");
            probe = 0;
            return false;
        }, context + " readiness after asynchronous reap", [&] {
            return QStringLiteral("probe request=%1 completed=%2").arg(probe).arg(results.count(probe));
        });
    };
    const auto capture = temp.filePath("capture");
    const QString text = QString::fromUtf8("中文\n🙂\t\"quoted\"");
    auto reply = result(request({{"text", "capture:" + capture + "\n" + text}, {"requestToken", "token"}}));
    Check(reply.value("ok").toBool() && reply.value("copy_state") == "committed" && reply.value("requestToken") == "token", "successful commit with token");
    QFile captured(capture); Check(captured.open(QIODevice::ReadOnly) && captured.readAll() == text.toUtf8(), "exact UTF-8 stdin and MIME args");
    Check(result(request({{"text", ""}})).value("ok").toBool(), "empty text supported");
    Check(result(request({{"text", QString(ClipboardEvents::MaxTextBytes, 'x')}})).value("ok").toBool(), "one MiB accepted");
    failure({{"text", QString(ClipboardEvents::MaxTextBytes / 3 + 1, QChar(0x4e2d))}}, "too_large");
    failure({{"text", 42}}, "invalid_request");
    failure({{"text", "x"}, {"requestToken", 1}}, "invalid_request");
    failure({{"text", "x"}, {"extra", true}}, "invalid_request");
    failure({{"text", "x"}}, "invalid_request", false, "other");
    for (const QString& json : {QStringLiteral("{\"command\":\"other\",\"text\":\"x\"}"),
             QStringLiteral("{\"command\":\"write_clipboard\",\"text\":\"a\",\"te\\u0078t\":\"b\"}"),
             QStringLiteral("{\"command\":\"write_clipboard\",\"text\":42}"), QStringLiteral("[]"),
             QStringLiteral("{\"command\":\"write_clipboard\",\"text\":\"x\",\"extra\":{\"a\":1,\"a\":2}}")})
        failure({{"json", json}}, "invalid_request", true);
    failure({{"json", 42}}, "invalid_request", true);
    failure({{"json", "{}"}}, "invalid_request", true, "other");
    reply = result(request({{"json", QString::fromUtf8(QJsonDocument(QJsonObject{{"command", "write_clipboard"}, {"text", text}}).toJson())}}, true));
    Check(reply.value("ok").toBool() && reply.value("copy_state") == "committed", "external command result JSON");
    const auto gatewayJson = QString::fromUtf8(QJsonDocument(QJsonObject{{"command", "write_clipboard"}, {"text", text}}).toJson());
    for (const QString& connection : {QString(), QStringLiteral("ipc-session-123")}) {
        reply = result(request({{"json", gatewayJson}, {"connection", connection}}, true));
        Check(reply.value("ok").toBool() && !reply.contains("connection"), "real gateway envelope accepts and ignores routing metadata");
    }
    failure({{"json", gatewayJson}, {"connection", 42}}, "invalid_request", true);
    failure({{"json", gatewayJson}, {"extra", "unknown"}}, "invalid_request", true);
    failure({{"json", gatewayJson}, {"connection", "ipc"}, {"extra", "unknown"}}, "invalid_request", true);
    failure({{"text", "fail"}}, "backend_failed");
    failure({{"text", "crash"}}, "backend_failed");
    const auto writeErrorPath = temp.filePath("write-error");
    qputenv("DESKWAIFU_CLIPBOARD_FIXTURE_CLOSE_STDIN", writeErrorPath.toUtf8());
    QElapsedTimer writeErrorTimer; writeErrorTimer.start();
    reply = result(request({{"text", QString(ClipboardEvents::MaxTextBytes, 'x')}}));
    qunsetenv("DESKWAIFU_CLIPBOARD_FIXTURE_CLOSE_STDIN");
    Check(reply.value("error_code") == "write_failed" && reply.value("copy_state") == "unknown" && writeErrorTimer.elapsed() < 2000,
        "asynchronous WriteError terminates before deadline");
    Wait([&] { return Pid(writeErrorPath) && Dead(Pid(writeErrorPath)); }, "WriteError fixture exit", [&] {
        return QStringLiteral("pid=%1 exists=%2").arg(Pid(writeErrorPath)).arg(QFile::exists(writeErrorPath));
    });
    ready("WriteError");
    auto hung = [&](const QString& name) {
        const auto path = temp.filePath(name);
        const auto id = request({{"text", "tree:" + path}});
        Wait([&] {
            if (results.count(id)) {
                const auto& response = results.at(id);
                const auto code = response.payload ? response.payload->data.value("error_code").toString() : QString();
                throw std::runtime_error(QStringLiteral("%1 fixture terminated before ready: request=%2 status=%3 error_code=%4")
                    .arg(name).arg(id).arg(RequestStatusName(response.status)).arg(code).toStdString());
            }
            return Pid(path) != 0 && Pid(path + ".child") != 0;
        }, name + " fixture parent/child ready", [&] {
            return QStringLiteral("request=%1 parent=%2 child=%3").arg(id).arg(Pid(path)).arg(Pid(path + ".child"));
        });
        return std::make_pair(id, Pid(path));
    };
    auto active = hung("cancel");
    failure({{"text", "not-started"}}, "busy");
    Check(client->Cancel(active.first), "cancel active request");
    Wait([&] { return results.count(active.first) && kill(active.second, 0) == -1 && errno == ESRCH; }, "cancel response and direct child reap");
    Wait([&] { return Dead(Pid(temp.filePath("cancel.child"))); }, "cancel descendant exit");
    ready("cancel");
    Check(results.at(active.first).status == RequestStatus::Cancelled, "cancel stays transport cancellation, never claims rollback");
    Check(result(request({{"text", "after-cancel"}})).value("ok").toBool(), "new request after reap");
    active = hung("deadline");
    QElapsedTimer heartbeat; heartbeat.start();
    bool ticked = false; QTimer::singleShot(5, [&] { ticked = true; });
    reply = result(active.first);
    Check(ticked && heartbeat.elapsed() < 4500 && reply.value("error_code") == "timeout" && reply.value("copy_state") == "unknown", "3 second deadline is asynchronous and uncertain");
    Wait([&] { return kill(active.second, 0) == -1 && errno == ESRCH; }, "deadline direct child reap");
    Wait([&] { return Dead(Pid(temp.filePath("deadline.child"))); }, "deadline descendant exit");
    ready("deadline");
    active = hung("stop");
    provider->Invalidate(); heartbeat.restart(); service->Stop(); service.reset();
    Check(heartbeat.elapsed() < 100, "Stop and destructor do not block");
    Wait([&] { return kill(active.second, 0) == -1 && errno == ESRCH; }, "Stop direct child reap");
    Wait([&] { return Dead(Pid(temp.filePath("stop.child"))); }, "Stop descendant exit");
    Check(results.at(active.first).status == RequestStatus::EndpointUnavailable, "stop invalidation result");
    ClipboardService* missingBridge = nullptr;
    auto missingProvider = loop.CreateEndpoint("clipboard-test/missing", [&](const EventDelivery& delivery) { missingBridge->Deliver(delivery); });
    Check(missingProvider->Provide(ClipboardEvents::WriteTextCapability), "missing provider capability");
    ClipboardService unavailable(*missingProvider, temp.filePath("missing-program")); missingBridge = &unavailable;
    failure({{"text", "x"}}, "unavailable");
    missingProvider->Invalidate(); unavailable.Stop();
    std::cout << "Clipboard async contracts passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
