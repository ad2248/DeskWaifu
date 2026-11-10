#include "VoiceInputController.hpp"
#include "audio/AudioCapture.hpp"
#include "asr/NativeAsrClient.hpp"
#include "contracts/VoiceInputEvents.hpp"
#include "event/ProcessorEventLoop.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 4000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "controller wait timed out");
}
void Drain() { for (int i = 0; i < 100; ++i) QCoreApplication::processEvents(); }
struct Capture : AudioCapture {
    bool startOk = true, stopOk = true;
    int starts = 0, stops = 0, cancels = 0;
    quint64 id = 0;
    QByteArray pcm = QByteArray(320, '\0');
    bool Start(quint64 session, const QString&, int, QString&) override { id = session; ++starts; return startOk; }
    bool Stop(quint64, QByteArray& data, QString&) override { ++stops; data = pcm; return stopOk; }
    void Cancel() override { ++cancels; }
};
struct Asr : NativeAsrClient {
    int starts = 0, cancels = 0;
    quint64 id = 0;
    QString key;
    void Recognize(quint64 session, const QByteArray&, const VoiceInputConfig& settings) override
    { ++starts; id = session; key = settings.apiKey; }
    void Cancel() override { ++cancels; }
};
}
int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory; Check(directory.isValid(), "private settings directory");
    VoiceInputSettings settings(directory.path()); QString error;
    Check(settings.Set(std::string(R"({"api_key":"test-secret","max_recording_seconds":1})"), error), "configure settings");
    ProcessorEventLoop loop;
    Capture capture; Asr asr;
    VoiceInputController* controller = nullptr;
    auto endpoint = loop.CreateEndpoint("voice-test/provider", [&](const EventDelivery& delivery) {
        if (delivery.kind == EventDelivery::Kind::Cancellation) controller->CancelRequest(delivery.requestId);
        else if (delivery.kind == EventDelivery::Kind::Request) controller->Recognize(delivery.requestId, delivery.payload->data.value("pcm").toByteArray());
    });
    Check(endpoint->Provide(VoiceInputEvents::Recognize), "register recognize");
    VoiceInputController actual(*endpoint, settings, capture, asr); controller = &actual;
    auto state = [&] { return actual.Status().value("state").toString(); };
    auto session = [&] { return actual.Status().value("session_id").toString(); };
    int copies = 0; RequestId pendingCopy = 0; QString copied;
    bool respondCopy = true, copySuccess = true;
    auto desktop = loop.CreateEndpoint("voice-test/desktop", [&](const EventDelivery& delivery) {
        if (delivery.kind != EventDelivery::Kind::Request) return;
        ++copies; pendingCopy = delivery.requestId; copied = delivery.payload->data.value("text").toString();
    });
    // Qt queued callback avoids capturing an auto variable inside its own initializer.
    auto respond = [&] {
        if (pendingCopy && respondCopy) {
            const auto id = pendingCopy; pendingCopy = 0;
            desktop->Respond(id, std::make_shared<ProcessorEvent>("clipboard.write_result", nullptr, nullptr,
                QVariantMap{{"ok", copySuccess}, {"copy_state", copySuccess ? "committed" : "failed"}}));
        }
    };
    Check(desktop->Provide("desktop.clipboard.write_text/v1"), "register clipboard");
    Check(actual.Toggle().value("ok").toBool() && state() == "recording", "first toggle records");
    const auto firstId = capture.id;
    const auto firstSession = session();
    Check(!actual.StartRecording().value("ok").toBool() && capture.starts == 1, "duplicate start busy");
    Check(actual.Toggle().value("ok").toBool() && state() == "transcribing", "second toggle transcribes");
    Check(!actual.Toggle().value("ok").toBool() && asr.starts == 1, "toggle during recognition rejected");
    Check(actual.StopRecording(firstSession).value("ok").toBool() && asr.starts == 1, "duplicate stop idempotent");
    emit capture.Failed(firstId, "late_capture_failure", "late capture failure");
    Check(state() == "transcribing", "completed capture cannot fail a later pipeline stage");
    emit asr.Recognized(firstId, QString::fromUtf8(" 中文\n原文 "));
    Wait([&] { return copies == 1; }); respond(); Wait([&] { return state() == "idle"; });
    Check(copied == QString::fromUtf8(" 中文\n原文 ") && actual.Status().value("last_outcome") == "copied", "original text copied once");
    Check(!actual.Status().contains("text") && !actual.Status().contains("api_key"), "status has no sensitive text/key");
    emit asr.Recognized(firstId, "late"); Drain(); Check(copies == 1, "late success ignored");

    capture.startOk = false;
    Check(!actual.StartRecording().value("ok").toBool() && state() == "idle", "start failure resets");
    capture.startOk = true; actual.StartRecording();
    capture.stopOk = false;
    Check(!actual.StopRecording(session()).value("ok").toBool() && state() == "idle", "stop failure resets timer/state");
    capture.stopOk = true;
    actual.StartRecording(); const auto cancelled = capture.id; actual.StopRecording(session());
    actual.Cancel(session());
    emit asr.Recognized(cancelled, "must not copy"); Drain(); Check(copies == 1, "cancel prevents new copy");
    actual.StartRecording();
    emit capture.Failed(cancelled, "old_device", "old");
    Check(state() == "recording", "stale device failure cannot hit new session");
    emit capture.Failed(capture.id, "device_lost", "设备断开");
    Check(state() == "idle" && actual.Status().value("error_code") == "device_lost", "active device failure resets");

    actual.StartRecording(); actual.StopRecording(session());
    emit asr.Recognized(asr.id, "  \n "); Check(state() == "idle" && copies == 1, "blank text never copies");
    actual.StartRecording(); actual.StopRecording(session());
    copySuccess = false; emit asr.Recognized(asr.id, "retained");
    Wait([&] { return pendingCopy != 0; }); respond(); Wait([&] { return state() == "idle"; });
    Check(actual.Result(session()).value("text") == "retained" && actual.Status().value("last_outcome") == "recognized_not_copied", "copy failure preserves text");

    copySuccess = true;
    const auto retainedSession = session();
    Check(actual.RetryCopy(retainedSession).value("ok").toBool(), "retry reuses retained backend result");
    Wait([&] { return pendingCopy != 0; }); respond(); Wait([&] { return state() == "idle"; });
    Check(copied == "retained" && actual.Status().value("last_outcome") == "copied", "retry copies without recognizing again");
    Check(!actual.RetryCopy(firstSession).value("ok").toBool(), "old-session retry rejected");
    actual.StartRecording(); actual.StopRecording(session());
    const QString largeText = QString(70000, '\n') + QString::fromUtf8("长结果");
    emit asr.Recognized(asr.id, largeText);
    Wait([&] { return pendingCopy != 0; }); respond(); Wait([&] { return state() == "idle"; });
    Check(actual.RetryCopy(session()).value("ok").toBool(), "large result retry accepted by session");
    Wait([&] { return pendingCopy != 0; }); respond(); Wait([&] { return state() == "idle"; });
    Check(copied == largeText, "large escaped result retried on internal bus without IPC text upload");
    respondCopy = false; actual.StartRecording(); actual.StopRecording(session());
    emit asr.Recognized(asr.id, "in flight"); Wait([&] { return pendingCopy != 0; });
    const auto copyId = pendingCopy; pendingCopy = 0;
    actual.Cancel(session()); Check(actual.Status().value("copy_state") == "unknown", "copy cancellation admits unknown side effect");
    Check(!desktop->Respond(copyId, std::make_shared<ProcessorEvent>("clipboard.write_result")), "cancel retracts bus request");
    // Retry the same session before the old Cancelled completion can be dispatched.
    Check(actual.RetryCopy(session()).value("ok").toBool(), "retry immediately after cancel");
    Wait([&] { return pendingCopy != 0; }); Drain();
    Check(actual.Status().value("phase") == "copying", "old cancelled completion cannot finish same-session retry");
    respondCopy = true; copySuccess = true; respond(); Wait([&] { return state() == "idle"; });
    actual.RetryCopy(session()); Wait([&] { return pendingCopy != 0; });
    respond(); // The old successful completion is queued, but not dispatched yet.
    actual.Cancel(session());
    Check(actual.RetryCopy(session()).value("ok").toBool(), "retry with old success already queued");
    Wait([&] { return pendingCopy != 0; }); Drain();
    Check(actual.Status().value("phase") == "copying", "old successful completion cannot finish replacement copy");
    respond(); Wait([&] { return state() == "idle"; });

    auto requester = loop.CreateEndpoint("voice-test/requester", [](const EventDelivery&) {});
    std::map<RequestId, RequestResult> results;
    auto recognize = [&](int timeout = 2000) {
        return requester->Request(VoiceInputEvents::Recognize,
            std::make_shared<ProcessorEvent>(VoiceInputEvents::Audio, nullptr, nullptr, QVariantMap{{"pcm", QByteArray(100, '\0')}}),
            [&](const RequestResult& result) { results.emplace(result.requestId, result); }, timeout);
    };
    auto request = recognize(); const auto starts = asr.starts;
    Wait([&] { return asr.starts == starts + 1; });
    const auto directId = asr.id;
    Check(!actual.StartRecording().value("ok").toBool(), "direct recognize owns shared task slot");
    auto other = recognize(); Wait([&] { return results.count(other); });
    Check(!results.at(other).payload->data.value("ok").toBool(), "second direct recognize busy");
    Check(requester->Cancel(request), "cancel direct request");
    Wait([&] { return state() == "idle"; });
    actual.StartRecording();
    actual.CancelRequest(request); emit asr.Recognized(directId, "old");
    Check(state() == "recording", "old request cancellation/success cannot stop new capture");
    actual.Cancel(session());
    request = recognize(); Wait([&] { return state() == "transcribing"; });
    const auto countBeforeDirect = copies; emit asr.Recognized(asr.id, "direct only");
    Wait([&] { return results.count(request); });
    Check(results.at(request).payload->data.value("text") == "direct only" && copies == countBeforeDirect, "direct recognize does not copy");
    request = recognize(15); Wait([&] { return state() == "transcribing"; });
    const auto expiredId = asr.id;
    Wait([&] { return results.count(request) && state() == "idle"; });
    Check(results.at(request).status == RequestStatus::TimedOut, "request deadline cancels real recognition");
    emit asr.Recognized(expiredId, "expired"); Drain(); Check(copies == countBeforeDirect, "expired result never copies");
    auto disappearing = loop.CreateEndpoint("voice-test/disappearing", [](const EventDelivery&) {});
    disappearing->Request(VoiceInputEvents::Recognize,
        std::make_shared<ProcessorEvent>(VoiceInputEvents::Audio, nullptr, nullptr, QVariantMap{{"pcm", QByteArray(100, '\0')}}),
        [](const RequestResult&) { throw std::runtime_error("destroyed requester callback"); });
    Wait([&] { return state() == "transcribing"; });
    disappearing.reset(); Wait([&] { return state() == "idle"; });
    actual.StartRecording(); const auto timeoutStart = asr.starts;
    Wait([&] { return asr.starts > timeoutStart; });
    Check(state() == "transcribing", "recording limit stops and recognizes"); actual.Cancel(session());
    desktop.reset(); actual.StartRecording(); actual.StopRecording(session()); emit asr.Recognized(asr.id, "no desktop");
    Wait([&] { return state() == "idle"; });
    Check(actual.Status().value("last_outcome") == "recognized_not_copied", "missing desktop is recoverable");
    actual.StartRecording(); const auto shutdownId = capture.id;
    endpoint->Invalidate(); actual.Shutdown(); emit asr.Recognized(shutdownId, "after stop"); Drain();
    Check(!actual.StartRecording().value("ok").toBool() && state() == "idle", "shutdown releases resources without live endpoint");
    std::cout << "Voice input control/state/cancellation/copy contracts passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
