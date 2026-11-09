#include "plugin/PluginManager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), message);
}
struct Reply { int calls = 0; RequestStatus status; QJsonObject json; };
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2 || argc == 3, "expected the real desktop adapter and optional early client");
    QTemporaryDir directory;
    Check(directory.isValid() && QDir().mkpath(directory.path() + "/plugins/desktop"), "private plugin root");
    Check(QFile::copy(app.arguments()[1], directory.path() + "/plugins/desktop/desktop.so"), "install desktop fixture");
    if (argc == 3) {
        Check(QDir().mkpath(directory.path() + "/plugins/a"), "private early client directory");
        Check(QFile::copy(app.arguments()[2], directory.path() + "/plugins/a/a.so"), "install earlier-starting client");
    }
    ProcessorEventLoop loop;
    PluginManager plugins(loop, directory.path() + "/plugins", directory.path() + "/config");
    QString error;
    Check(plugins.Start(&error), qPrintable(error));
    Check(plugins.LoadedPlugins() == (argc == 3 ? QStringList{"a", "desktop"} : QStringList{"desktop"}),
        "load the real desktop entry through the public host in name order");
    auto client = loop.CreateEndpoint("desktop-test-client", [](const EventDelivery&) {});
    if (argc == 3) {
        QJsonArray records;
        bool done = false;
        Check(client->Request("fixture.desktop.early-results/v1", std::make_shared<ProcessorEvent>("fixture.results"),
            [&](const RequestResult& result) {
                Check(result.status == RequestStatus::Replied && result.payload, "read early client results");
                records = result.payload->data.value("records").toJsonArray(); done = true;
            }) != 0, "query early results through the public SDK");
        Wait([&] { return done; }, "early client results timed out");
        Check(records.size() == 6, "probe both domains before Start, during Initialize and during StartFrames");
        for (const auto& phase : QStringList{"before_start", "initialize", "frames"}) {
            for (const auto& command : QStringList{"get_status", "set_model"}) {
                int matches = 0;
                for (const auto& record : records) {
                    const auto sample = record.toObject();
                    if (sample.value("phase") != phase || sample.value("command") != command) continue;
                    ++matches;
                    Check(sample.value("status") == "replied" && sample.value("response").toObject() ==
                        QJsonObject{{"ok", false}, {"error", "desktop is not ready"}},
                        "both domains must reject early commands explicitly without queuing a lost result");
                }
                Check(matches == 1, "each early command completes exactly once in its own startup phase");
            }
        }
    }
    std::map<RequestId, Reply> replies;
    auto request = [&](const QString& capability, const QString& command, const QString& tag, bool hold = false) {
        const auto json = QJsonDocument(QJsonObject{{"command", command}, {"tag", tag}, {"hold", hold}}).toJson(QJsonDocument::Compact);
        const auto id = client->Request("command." + capability + "/v1",
            std::make_shared<ProcessorEvent>("command", nullptr, nullptr, QVariantMap{{"json", QString::fromUtf8(json)}}),
            [&](const RequestResult& result) {
                auto& reply = replies[result.requestId];
                ++reply.calls; reply.status = result.status;
                if (result.payload) reply.json = QJsonDocument::fromJson(result.payload->data.value("json").toString().toUtf8()).object();
            });
        Check(id != 0, "enqueue public desktop request");
        return id;
    };
    auto received = [&](RequestId id, RequestStatus status) -> const Reply& {
        Wait([&] { return replies.count(id) != 0; }, "desktop request did not finish before the held domain result");
        Check(replies.at(id).calls == 1 && replies.at(id).status == status, "request must finish exactly once with the expected status");
        return replies.at(id);
    };
    auto control = [&](const QString& operation = "snapshot") {
        QVariantMap state;
        bool done = false;
        Check(client->Request("fixture.desktop/v1",
            std::make_shared<ProcessorEvent>("fixture", nullptr, nullptr, QVariantMap{{"operation", operation}}),
            [&](const RequestResult& result) {
                Check(result.status == RequestStatus::Replied && result.payload, "runtime fixture control completion");
                state = result.payload->data; done = true;
            }) != 0, "enqueue runtime control");
        Wait([&] { return done; }, "runtime fixture control timed out");
        return state;
    };
    auto seen = [&](const QString& domain, const QStringList& tags) {
        QVariantMap state;
        Wait([&] { state = control(); return state.value(domain).toStringList() == tags; }, "unexpected domain dispatch ordering");
        return state;
    };

    const auto cancelledModel = request("set_model", "set_model", "model-cancelled");
    seen("models", {"model-cancelled"});
    const auto skippedModel = request("set_model", "set_model", "model-skipped");
    const auto nextModel = request("get_hotkeys", "get_hotkeys", "model-next");
    const auto lastModel = request("next_model", "next_model", "model-last");
    control(); // Deliver all requests into DesktopCommands before cancelling its queued item.
    Check(client->Cancel(cancelledModel) && client->Cancel(skippedModel), "cancel active and unstarted model requests");
    received(cancelledModel, RequestStatus::Cancelled);
    received(skippedModel, RequestStatus::Cancelled);
    const auto mismatchModel = request("set_model", "get_status", "mismatch-model");
    const auto mismatchInteraction = request("get_status", "set_model", "mismatch-interaction");
    const auto status = request("get_status", "get_status", "status");
    const auto toggle = request("toggle_hidden", "toggle_hidden", "toggle");
    const auto quit = request("quit", "quit", "quit");
    Check(received(status, RequestStatus::Replied).json.value("tag") == "status", "status progresses independently of cancelled model work");
    Check(received(toggle, RequestStatus::Replied).json.value("hidden").toBool(), "toggle progresses before model completion");
    Check(received(quit, RequestStatus::Replied).json.value("tag") == "quit", "quit progresses before model completion");
    for (const auto id : {mismatchModel, mismatchInteraction})
        Check(received(id, RequestStatus::Replied).json.value("error") == "command capability mismatch", "malformed capability cannot stall either queue");
    auto state = control();
    Check(state.value("models").toStringList() == QStringList{"model-cancelled"} &&
        state.value("interactions").toStringList() == QStringList{"status", "toggle", "quit"} &&
        state.value("quits").toInt() == 1, "held model work stays paired while all interaction commands execute");
    Check(!state.value("completed").toStringList().contains("model-cancelled") &&
        !replies.count(nextModel) && !replies.count(lastModel), "no model completion may be fabricated on cancellation");

    Check(control("complete_model").value("posted").toBool(), "complete already-executing cancelled mutation");
    state = seen("models", {"model-cancelled", "model-next"});
    Check(state.value("completed").toStringList().contains("model-cancelled") && !replies.count(nextModel),
        "cancelled mutation still completes but its result must not satisfy the next caller");
    received(cancelledModel, RequestStatus::Cancelled);
    received(skippedModel, RequestStatus::Cancelled);
    Check(control("complete_model").value("posted").toBool(), "complete next model");
    Check(received(nextModel, RequestStatus::Replied).json.value("tag") == "model-next", "next model result retains correlation");
    seen("models", {"model-cancelled", "model-next", "model-last"});
    Check(control("complete_model").value("posted").toBool(), "complete later model");
    Check(received(lastModel, RequestStatus::Replied).json.value("tag") == "model-last", "later model result retains correlation");

    const auto cancelledInteraction = request("get_status", "get_status", "interaction-cancelled", true);
    seen("interactions", {"status", "toggle", "quit", "interaction-cancelled"});
    const auto skippedInteraction = request("toggle_hidden", "toggle_hidden", "interaction-skipped");
    const auto nextInteraction = request("get_status", "get_status", "interaction-next");
    control(); // Cancellation must exercise the adapter's queue, not just the SDK delivery fence.
    Check(client->Cancel(cancelledInteraction) && client->Cancel(skippedInteraction), "cancel active and unstarted interaction requests");
    received(cancelledInteraction, RequestStatus::Cancelled);
    received(skippedInteraction, RequestStatus::Cancelled);
    const auto parallelModel = request("get_hotkeys", "get_hotkeys", "model-parallel");
    seen("models", {"model-cancelled", "model-next", "model-last", "model-parallel"});
    Check(control("complete_model").value("posted").toBool(), "model domain also progresses while interaction is held");
    Check(received(parallelModel, RequestStatus::Replied).json.value("tag") == "model-parallel", "model result cannot consume the interaction tombstone");
    Check(!replies.count(nextInteraction), "held interaction tombstone preserves pairing");
    Check(control("complete_interaction").value("posted").toBool(), "release interaction tombstone");
    Check(received(nextInteraction, RequestStatus::Replied).json.value("tag") == "interaction-next", "next interaction result retains correlation");
    received(cancelledInteraction, RequestStatus::Cancelled);
    received(skippedInteraction, RequestStatus::Cancelled);
    state = control();
    Check(state.value("hidden").toBool() && !state.value("interactions").toStringList().contains("interaction-skipped"),
        "unstarted cancelled interaction mutation must never execute");

    const auto stoppedModel = request("set_model", "set_model", "stop-model");
    const auto stoppedInteraction = request("get_status", "get_status", "stop-interaction", true);
    seen("models", {"model-cancelled", "model-next", "model-last", "model-parallel", "stop-model"});
    seen("interactions", {"status", "toggle", "quit", "interaction-cancelled", "interaction-next", "stop-interaction"});
    const auto queuedModel = request("next_model", "next_model", "stop-queued-model");
    const auto queuedInteraction = request("toggle_hidden", "toggle_hidden", "stop-queued-interaction");
    state = control("stop");
    Check(state.value("posted").toBool() && !state.value("registered").toBool(), "Stop must unregister the real command processor and fence an already-posted result");
    for (const auto id : {stoppedModel, stoppedInteraction, queuedModel, queuedInteraction}) received(id, RequestStatus::EndpointUnavailable);
    received(request("get_status", "get_status", "after-stop"), RequestStatus::NoProvider);
    Check(!control("complete_model").value("posted").toBool() && !control("complete_interaction").value("posted").toBool(), "late domain results cannot target a stopped processor");
    state = control();
    Check(!state.value("models").toStringList().contains("stop-queued-model") &&
        !state.value("interactions").toStringList().contains("stop-queued-interaction"), "Stop discards unstarted work in both domains");
    Check(plugins.Stop(&error), qPrintable(error));
    std::cout << "Real desktop adapter independent domains, cancellation pairing, mismatch and Stop passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
