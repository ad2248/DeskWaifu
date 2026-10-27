#include "processor/speech/SpeechProcessor.hpp"
#include "processor/speech/source/MiniMaxSpeechSource.hpp"
#include "processor/speech/audio/AudioPlayer.hpp"
#include "processor/Events.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>

struct Receiver final : Processor
{
    using Processor::Processor;
    std::vector<ProcessorEvent> events;
    bool HandleEvent(const ProcessorEvent& event) override
    { if (event.receiver != this) return false; events.push_back(event); return true; }
};
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    MiniMaxSpeechSource source(QString{}); AudioPlayer audio; SpeechPlayer player(source, audio);
    SpeechProcessor speech(loop, player); Receiver receiver(loop);
    loop.Post({ProcessorEvents::Speak, &receiver, &speech, {{"text", "test without credentials"}}});
    QElapsedTimer timer; timer.start();
    while (receiver.events.empty() && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    const bool ok = receiver.events.size() == 1 && receiver.events[0].type == ProcessorEvents::Failed &&
        !receiver.events[0].data.value("error").toString().isEmpty();
    if (!ok) std::cerr << "speech failure returned as event without cloud or playback\n";
    return ok ? 0 : 1;
}
