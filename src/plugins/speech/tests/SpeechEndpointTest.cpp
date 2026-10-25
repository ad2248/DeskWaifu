#include "processor/speech/SpeechEndpoint.hpp"
#include "processor/speech/source/MiniMaxSpeechSource.hpp"
#include "processor/speech/audio/AudioPlayer.hpp"
#include "contracts/SpeechEvents.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "speech endpoint condition timed out");
}
void Drain() { for (int i = 0; i < 100; ++i) QCoreApplication::processEvents(); }
struct Source : MiniMaxSpeechSource
{
    Source() : MiniMaxSpeechSource({}) {}
    std::vector<SpeechId> starts, cancels;
    QStringList texts;
    void Start(SpeechId id, SpeechRequest request) override { starts.push_back(id); texts.append(request.text); }
    void Cancel(SpeechId id) override { cancels.push_back(id); }
};
struct Audio : AudioPlayer
{
    std::vector<SpeechId> stops, drains;
    QByteArray pcm;
    void Start(SpeechId, QAudioFormat) override {}
    void Append(SpeechId, QByteArray data) override { pcm += data; }
    void Stop(SpeechId id) override { stops.push_back(id); pcm.clear(); }
    void FinishInput(SpeechId id) override { drains.push_back(id); }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    Source source; Audio audio; SpeechPlayer player(source, audio);
    SpeechEndpoint* bridge = nullptr;
    auto provider = loop.CreateEndpoint("speech-test/provider", [&](const EventDelivery& message) {
        if (message.kind == EventDelivery::Kind::Request) bridge->Speak(message.requestId, message.payload->data);
        if (message.kind == EventDelivery::Kind::Cancellation) bridge->Cancel(message.requestId);
    });
    provider->Provide("speech.speak/v1");
    auto adapter = std::make_unique<SpeechEndpoint>(loop, *provider, player);
    bridge = adapter.get();
    auto client = loop.CreateEndpoint("speech-test/client", [](const EventDelivery&) {});
    std::map<RequestId, RequestResult> results;
    auto speak = [&](const QString& text, bool replace = false, int timeout = 30000) {
        return client->Request("speech.speak/v1", std::make_shared<ProcessorEvent>(SpeechEvents::Speak, nullptr, nullptr,
            QVariantMap{{"text", text}, {"replace", replace}}),
            [&](const RequestResult& result) { Check(!results.count(result.requestId), "only one terminal result per request"); results.emplace(result.requestId, result); }, timeout);
    };
    auto finish = [&](SpeechId id) {
        emit source.Ready(id, {}); emit source.AudioReceived(id, "pcm"); emit source.Completed(id);
        Wait([&] { return !audio.drains.empty() && audio.drains.back() == id; });
        emit audio.Drained(id);
    };

    auto first = speak("normal");
    Wait([&] { return source.starts.size() == 1; });
    finish(source.starts.back()); Wait([&] { return results.count(first); });
    Check(results.at(first).status == RequestStatus::Replied && results.at(first).payload->type == SpeechEvents::Completed, "legacy completion maps to the exact bus request");

    first = speak("cancel-after-terminal");
    Wait([&] { return source.texts.back() == "cancel-after-terminal"; });
    auto old = source.starts.back();
    Check(client->Cancel(first), "cancel active bus request");
    Wait([&] { return results.count(first); });
    Check(results.at(first).status == RequestStatus::Cancelled, "bus cancellation terminal");
    Wait([&] { return std::count(source.cancels.begin(), source.cancels.end(), old) == 1; });
    Check(std::count(audio.stops.begin(), audio.stops.end(), old) == 1, "audio and network still stop after bus request is terminal");
    emit source.Completed(old); emit audio.Drained(old); Drain();
    Check(!provider->Respond(first, std::make_shared<ProcessorEvent>()), "late legacy completion cannot resurrect cancelled bus request");

    first = speak("old"); Wait([&] { return source.texts.back() == "old"; });
    old = source.starts.back();
    const auto second = speak("replacement", true);
    Wait([&] { return results.count(first) && source.texts.back() == "replacement"; });
    const auto current = source.starts.back();
    Check(results.at(first).status == RequestStatus::Replied && results.at(first).payload->type == "operation.failed", "replacement fails only the old correlated request");
    bridge->Cancel(first); // A delayed old cancellation must not hit this adapter's new request.
    emit source.Ready(old, {}); emit source.AudioReceived(old, "late"); emit source.Completed(old); emit audio.Drained(old);
    Drain();
    Check(player.CurrentId() == current && audio.pcm.isEmpty(), "old request ID and stale backend signals cannot stop or contaminate replacement");
    finish(current); Wait([&] { return results.count(second); });
    Check(results.at(second).payload->type == SpeechEvents::Completed, "replacement receives its own completion");

    first = speak("repeat-old"); Wait([&] { return source.texts.back() == "repeat-old"; });
    const auto superseded = speak("repeat-middle", true);
    const auto latest = speak("repeat-latest", true);
    Wait([&] { return results.count(first) && results.count(superseded) && source.texts.back() == "repeat-latest"; });
    Check(results.at(first).payload->type == "operation.failed" && results.at(superseded).payload->type == "operation.failed",
          "repeated replacements sharing one adapter retain distinct request IDs");
    finish(source.starts.back()); Wait([&] { return results.count(latest); });
    Check(results.at(latest).payload->type == SpeechEvents::Completed, "only latest repeated replacement succeeds");

    // Drive two bridge calls in one main-thread turn so the replacement is
    // definitely still pending when cancellation arrives (before old Finished).
    bridge->Speak(1000001, {{"text", "pending-old"}});
    Wait([&] { return source.texts.back() == "pending-old"; });
    const auto beforePending = source.starts.size();
    bridge->Speak(1000002, {{"text", "must-not-start"}, {"replace", true}});
    bridge->Cancel(1000002);
    Wait([&] { return player.CurrentState() == SpeechPlayer::State::Idle; }); Drain();
    Check(source.starts.size() == beforePending && !source.texts.contains("must-not-start"), "cancel removes a pending replacement before it can start");

    first = speak("timeout", false, 15);
    Wait([&] { return source.texts.back() == "timeout"; });
    old = source.starts.back();
    Wait([&] { return results.count(first); });
    Check(results.at(first).status == RequestStatus::TimedOut, "transport deadline terminates request");
    Wait([&] { return std::count(source.cancels.begin(), source.cancels.end(), old) == 1; });
    Check(std::count(audio.stops.begin(), audio.stops.end(), old) == 1, "timeout also stops actual backend work");

    auto disappearing = loop.CreateEndpoint("speech-test/disappearing", [](const EventDelivery&) {});
    const auto abandoned = disappearing->Request("speech.speak/v1",
        std::make_shared<ProcessorEvent>(SpeechEvents::Speak, nullptr, nullptr, QVariantMap{{"text", "abandoned"}}),
        [&](const RequestResult& result) { results.emplace(result.requestId, result); });
    Wait([&] { return source.texts.back() == "abandoned"; });
    old = source.starts.back(); disappearing.reset();
    Wait([&] { return std::count(source.cancels.begin(), source.cancels.end(), old) == 1; }); Drain();
    Check(!results.count(abandoned) && std::count(audio.stops.begin(), audio.stops.end(), old) == 1,
          "requester destruction stops actual work without invoking its completion");

    first = speak("stop"); Wait([&] { return source.texts.back() == "stop"; });
    old = source.starts.back();
    provider->Invalidate(); adapter->Stop(); adapter.reset();
    Wait([&] { return results.count(first); });
    Wait([&] { return player.CurrentState() == SpeechPlayer::State::Idle; });
    emit source.Completed(old); emit audio.Drained(old); Drain();
    Check(results.at(first).status == RequestStatus::EndpointUnavailable &&
          std::count(source.cancels.begin(), source.cancels.end(), old) == 1, "stop withdraws endpoint and releases backend without late callbacks");
    std::cout << "Speech endpoint correlation, cancellation, replacement and shutdown contracts passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
