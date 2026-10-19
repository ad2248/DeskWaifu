#include "SpeechPlayer.hpp"
#include "MiniMaxSpeechSource.hpp"
#include "AudioPlayer.hpp"
#include <QtTest>

class FakeSource : public MiniMaxSpeechSource
{
public:
    FakeSource() : MiniMaxSpeechSource({}) {}
    SpeechId started = 0, cancelled = 0;
    void Start(SpeechId id, SpeechRequest) override { started = id; }
    void Cancel(SpeechId id) override { cancelled = id; }
};

class FakeAudio : public AudioPlayer
{
public:
    SpeechId started = 0, stopped = 0, finished = 0;
    QByteArray received;
    void Start(SpeechId id, QAudioFormat) override { started = id; }
    void Append(SpeechId, QByteArray pcm) override { received += pcm; }
    void FinishInput(SpeechId id) override { finished = id; }
    void Stop(SpeechId id) override { stopped = id; received.clear(); }
};

class SpeechPlayerTest : public QObject
{
    Q_OBJECT
private slots:
    void completionWaitsForDrain()
    {
        FakeSource source;
        FakeAudio audio;
        SpeechPlayer player(source, audio);
        QSignalSpy done(&player, &SpeechPlayer::Finished);
        player.Speak({QStringLiteral("姐姐，你好喵"), {}});
        QTRY_VERIFY(player.CurrentId());
        const auto id = player.CurrentId();
        QCOMPARE(player.CurrentState(), SpeechPlayer::State::Preparing);
        emit source.Ready(id, {});
        emit source.AudioReceived(id, "pcm");
        QCOMPARE(audio.started, id);
        QCOMPARE(audio.received, QByteArray("pcm"));
        emit audio.PlaybackStarted(id);
        QTRY_COMPARE(player.CurrentState(), SpeechPlayer::State::Playing);
        emit source.Completed(id);
        QTRY_COMPARE(player.CurrentState(), SpeechPlayer::State::Draining);
        QCOMPARE(audio.finished, id);
        QCOMPARE(done.size(), 0);
        emit audio.PlaybackStarted(id); // Late device activation cannot undo draining.
        QCoreApplication::processEvents();
        QCOMPARE(player.CurrentState(), SpeechPlayer::State::Draining);
        emit audio.Drained(id);
        QTRY_COMPARE(player.CurrentState(), SpeechPlayer::State::Idle);
        QCOMPARE(done.size(), 1);
        QCOMPARE(qvariant_cast<SpeechPlayer::FinishReason>(done[0][1]), SpeechPlayer::FinishReason::Completed);
    }

    void replacementRejectsOldEvents()
    {
        FakeSource source;
        FakeAudio audio;
        SpeechPlayer player(source, audio);
        QSignalSpy done(&player, &SpeechPlayer::Finished);
        player.Speak({"first", {}});
        QTRY_VERIFY(source.started);
        const auto old = player.CurrentId();
        player.Speak({"second", {}});
        QTRY_VERIFY(player.CurrentId() != old);
        const auto current = player.CurrentId();
        QCOMPARE(source.cancelled, old);
        QCOMPARE(audio.stopped, old);
        QCOMPARE(done.size(), 1);
        QCOMPARE(qvariant_cast<SpeechPlayer::FinishReason>(done[0][1]), SpeechPlayer::FinishReason::Replaced);
        emit source.Ready(old, {});
        emit source.AudioReceived(old, "stale");
        emit source.Completed(old);
        emit source.Failed(old, {SpeechErrorCode::Network, "stale"});
        emit audio.Drained(old);
        QCoreApplication::processEvents();
        QCOMPARE(player.CurrentId(), current);
        QVERIFY(audio.received.isEmpty());
        player.Speak({"  ", {}});
        QCoreApplication::processEvents();
        QCOMPARE(player.CurrentId(), current);
        player.Stop();
        QTRY_COMPARE(player.CurrentState(), SpeechPlayer::State::Idle);
        QCOMPARE(source.cancelled, current);
        QCOMPARE(audio.stopped, current);
        QCOMPARE(done.size(), 2);
        player.Stop();
        QCoreApplication::processEvents();
        QCOMPARE(done.size(), 2);
    }

    void failureRecoversAndFinishesOnce()
    {
        FakeSource source;
        FakeAudio audio;
        SpeechPlayer player(source, audio);
        QSignalSpy errors(&player, &SpeechPlayer::ErrorOccurred);
        QSignalSpy done(&player, &SpeechPlayer::Finished);
        player.Speak({"test", {}});
        QTRY_VERIFY(player.CurrentId());
        const auto id = player.CurrentId();
        emit audio.Failed(id, {SpeechErrorCode::AudioDevice, "no device"});
        emit source.Failed(id, {SpeechErrorCode::Network, "disconnected"});
        QTRY_COMPARE(player.CurrentState(), SpeechPlayer::State::Idle);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(done.size(), 1);
        QCOMPARE(qvariant_cast<SpeechPlayer::FinishReason>(done[0][1]), SpeechPlayer::FinishReason::Failed);
        player.Speak({"again", {}});
        QTRY_VERIFY(player.CurrentId() > id);
        emit source.Ready(player.CurrentId(), {});
        emit source.Completed(player.CurrentId()); // Even a zero-audio task needs draining.
        QTRY_COMPARE(player.CurrentState(), SpeechPlayer::State::Draining);
        emit audio.Drained(player.CurrentId());
        QTRY_COMPARE(done.size(), 2);
    }

    void destructionCancelsPendingAudio()
    {
        FakeSource source;
        FakeAudio audio;
        SpeechId id;
        {
            SpeechPlayer player(source, audio);
            player.Speak({"test", {}});
            QTRY_VERIFY(player.CurrentId());
            id = player.CurrentId();
        }
        QCOMPARE(source.cancelled, id);
        QCOMPARE(audio.stopped, id);
    }
};

QTEST_GUILESS_MAIN(SpeechPlayerTest)
#include "SpeechPlayerTest.moc"
