#include "processor/Events.hpp"
#include "processor/speech/SpeechProcessor.hpp"
#include "processor/speech/source/MiniMaxSpeechSource.hpp"
#include "processor/speech/audio/AudioPlayer.hpp"
#include "EventLoopFence.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
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
struct Source : MiniMaxSpeechSource
{
    Source() : MiniMaxSpeechSource({}) {}
    std::vector<SpeechId> starts, cancels;
    void Start(SpeechId id, SpeechRequest) override { starts.push_back(id); std::cout << "source.start " << id << '\n'; }
    void Cancel(SpeechId id) override { cancels.push_back(id); std::cout << "source.cancel " << id << '\n'; }
};
struct Audio : AudioPlayer
{
    std::vector<SpeechId> starts, stops, drains;
    QByteArray pcm;
    void Start(SpeechId id, QAudioFormat) override { starts.push_back(id); }
    void Append(SpeechId, QByteArray data) override { pcm += data; }
    void Stop(SpeechId id) override { stops.push_back(id); pcm.clear(); }
    void FinishInput(SpeechId id) override { drains.push_back(id); }
};
struct Receiver : Processor
{
    Receiver(ProcessorEventLoop& loop, const char* name) : Processor(loop), name(name) {}
    const char* name;
    std::vector<ProcessorEvent> events;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.receiver != this) return false;
        events.push_back(event); std::cout << name << " <- " << event.type.toStdString() << '\n';
        return true;
    }
    void Fence()
    {
        Check(AwaitProcessorEvents(_loop), "FIFO completion fence not delivered");
    }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "expected one speech contract scenario");
    const QString scenario = app.arguments()[1];
    using namespace ProcessorEvents;
    ProcessorEventLoop loop;
    Source source; Audio audio; SpeechPlayer player(source, audio);
    auto speech = std::make_unique<SpeechProcessor>(loop, player);
    Receiver first(loop, "first"), second(loop, "second"), third(loop, "third");
    auto speak = [&](Receiver& receiver, bool replace = false) {
        Check(loop.Post({Speak, &receiver, speech.get(), {{"text", "synthetic"}, {"replace", replace}}}), "enqueue speech");
    };
    auto finish = [&](SpeechId id) {
        emit source.Ready(id, {});
        emit source.AudioReceived(id, "pcm");
        emit source.Completed(id);
        Wait([&] { return !audio.drains.empty() && audio.drains.back() == id; }, "EOF must request audio drain");
        emit audio.Drained(id);
    };
    auto terminal = [&](Receiver& receiver, const QString& type) {
        Wait([&] { return receiver.events.size() == 1; }, "request must receive one terminal event");
        Check(receiver.events[0].sender == speech.get() && receiver.events[0].receiver == &receiver && receiver.events[0].type == type,
            "terminal event type, sender and receiver must match the request");
    };
    if (scenario == "receiver-destroy") {
        auto receiver = std::make_unique<Receiver>(loop, "destroyed requester");
        speak(*receiver);
        Wait([&] { return source.starts.size() == 1; }, "request accepted before requester destruction");
        const auto id = source.starts[0];
        receiver.reset();
        finish(id);
        Wait([&] { return player.CurrentState() == SpeechPlayer::State::Idle; }, "orphaned task releases resources");
        first.Fence();
        Check(first.events.empty() && second.events.empty() && third.events.empty(), "dead requester's completion must not become a broadcast or call a dangling target");
        Check(source.cancels == std::vector<SpeechId>{id} && audio.stops == std::vector<SpeechId>{id}, "orphaned task releases source/audio once");
    } else {
        speak(first);
        Wait([&] { return source.starts.size() == 1; }, "initial speech accepted");
        const auto old = source.starts[0];
        if (scenario == "normal") {
            finish(old); terminal(first, Spoken);
        } else if (scenario == "cancel" || scenario == "cancel-playing" || scenario == "cancel-draining") {
            if (scenario != "cancel") {
                emit source.Ready(old, {}); emit audio.PlaybackStarted(old);
                Wait([&] { return player.CurrentState() == SpeechPlayer::State::Playing; }, "playback starts");
            }
            if (scenario == "cancel-draining") {
                emit source.Completed(old);
                Wait([&] { return player.CurrentState() == SpeechPlayer::State::Draining; }, "EOF enters draining");
            }
            loop.Post({Cancel, &second, speech.get()}); second.Fence();
            Check(source.cancels.empty(), "unrelated requester cannot cancel speech");
            loop.Post({Cancel, &first, speech.get()}); terminal(first, Failed);
            emit source.Completed(old); emit audio.Drained(old);
            Check(source.cancels == std::vector<SpeechId>{old} && audio.stops == std::vector<SpeechId>{old}, "cancel releases exactly once");
        } else if (scenario == "replace" || scenario == "repeat-replace" || scenario == "queued-replace") {
            if (scenario == "repeat-replace") {
                // 两次生产入口调用均在旧任务的异步 Finished 之前完成，
                // 精确控制“尚未启动的替换又被覆盖”的边界。
                speech->HandleEvent({Speak, &second, speech.get(), {{"text", "second"}, {"replace", true}}});
                speech->HandleEvent({Speak, &third, speech.get(), {{"text", "third"}, {"replace", true}}});
            } else {
                speak(second, true);
                if (scenario == "queued-replace") speak(third, true);
            }
            terminal(first, Failed);
            const bool multiple = scenario != "replace";
            if (multiple) terminal(second, Failed);
            Wait([&] { return source.starts.size() >= 2 && player.CurrentId(); }, "replacement starts after old task terminates");
            if (scenario == "repeat-replace") Check(source.starts.size() == 2, "superseded pending replacement must never start");
            const auto current = source.starts.back();
            Check(current != old, "replacement has a new task identity");
            emit source.Ready(old, {}); emit source.AudioReceived(old, "stale");
            emit source.Completed(old); emit source.Failed(old, {SpeechErrorCode::Network, "stale"}); emit audio.Drained(old);
            first.Fence();
            Check(player.CurrentId() == current && audio.starts.empty() && audio.pcm.isEmpty(), "old results cannot affect the new request");
            finish(current);
            terminal(multiple ? third : second, Spoken);
        } else if (scenario == "pending-destroy") {
            auto receiver = std::make_unique<Receiver>(loop, "pending replacement");
            speech->HandleEvent({Speak, receiver.get(), speech.get(), {{"text", "must not start"}, {"replace", true}}});
            receiver.reset(); // old Finished has not run yet
            terminal(first, Failed);
            first.Fence();
            Check(source.starts.size() == 1 && audio.starts.empty() && player.CurrentState() == SpeechPlayer::State::Idle,
                "destroyed pending requester must not start a source/audio task");
        } else if (scenario == "destroy") {
            speech.reset();
            emit source.Ready(old, {}); emit source.AudioReceived(old, "late"); emit source.Completed(old); emit audio.Drained(old);
            first.Fence();
            Check(first.events.empty() && source.cancels == std::vector<SpeechId>{old} && audio.stops == std::vector<SpeechId>{old}, "destroyed consumer cancels resources without dangling callbacks");
        } else throw std::runtime_error("unknown speech scenario");
        emit source.Completed(old); emit audio.Drained(old);
        loop.Post({Cancel, &first, speech.get()}); first.Fence();
        Check(first.events.size() == (scenario == "destroy" ? 0u : 1u), "duplicate terminal notifications must not be delivered");
    }
    std::cout << "speech contract passed: " << scenario.toStdString() << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << (argc > 1 ? argv[1] : "speech") << ": " << error.what() << '\n'; return 1;
}
