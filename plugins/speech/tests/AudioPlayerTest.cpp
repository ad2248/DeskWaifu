#include "processor/speech/audio/AudioPlayer.hpp"
#include "processor/speech/source/MiniMaxSpeechSource.hpp"
#include "processor/speech/SpeechPlayer.hpp"
#include <QFile>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QtTest>

class AudioPlayerTest : public QObject
{
    Q_OBJECT
    QAudioFormat Format()
    {
        QAudioFormat format;
        format.setSampleRate(24000);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
        return format;
    }
private slots:
    void invalidFormatFailsWithoutPlayback()
    {
        AudioPlayer audio;
        QSignalSpy errors(&audio, &AudioPlayer::Failed);
        audio.Start(1, {});
        QCOMPARE(errors.size(), 1);
        QCOMPARE(qvariant_cast<SpeechError>(errors[0][1]).code, SpeechErrorCode::AudioDevice);
    }

    void waitsForEndOfInputAndDrains()
    {
        if (!qEnvironmentVariableIsSet("DESKWAIFU_AUDIO_TEST")) QSKIP("Opt-in audio device test");
        AudioPlayer audio;
        QSignalSpy errors(&audio, &AudioPlayer::Failed);
        QSignalSpy done(&audio, &AudioPlayer::Drained);
        audio.Start(1, Format());
        audio.Append(1, QByteArray(1, '\0'));
        audio.Append(1, QByteArray(4799, '\0'));
        QTest::qWait(300); // 尚未到达 EOF 时，音频缓冲欠载不能结束任务。
        QCOMPARE(done.size(), 0);
        audio.Append(1, QByteArray(4800, '\0'));
        audio.FinishInput(1);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(done[0][0].toULongLong(), 1ULL);
    }

    void stopAndReplacementDiscardBufferedAudio()
    {
        if (!qEnvironmentVariableIsSet("DESKWAIFU_AUDIO_TEST")) QSKIP("Opt-in audio device test");
        AudioPlayer audio;
        QSignalSpy errors(&audio, &AudioPlayer::Failed);
        QSignalSpy done(&audio, &AudioPlayer::Drained);
        audio.Start(2, Format());
        audio.Append(2, QByteArray(48000, '\0'));
        audio.Stop(2);
        audio.Append(2, QByteArray(4800, '\0'));
        audio.FinishInput(2);
        QTest::qWait(100);
        QCOMPARE(done.size(), 0);
        audio.Start(3, Format());
        audio.Stop(2);
        audio.Append(2, "invalid");
        audio.FinishInput(3); // 零音频数据也应正常结束。
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(done[0][0].toULongLong(), 3ULL);
    }

    void rejectsPartialFramesAndOverflow()
    {
        if (!qEnvironmentVariableIsSet("DESKWAIFU_AUDIO_TEST")) QSKIP("Opt-in audio device test");
        AudioPlayer audio;
        QSignalSpy errors(&audio, &AudioPlayer::Failed);
        audio.Start(4, Format());
        audio.Append(4, "x");
        audio.FinishInput(4);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(qvariant_cast<SpeechError>(errors[0][1]).code, SpeechErrorCode::Protocol);
        audio.Start(5, Format());
        audio.Append(5, QByteArray(8 * 1024 * 1024 + 1, '\0'));
        QCOMPARE(errors.size(), 2);
        QCOMPARE(qvariant_cast<SpeechError>(errors[1][1]).code, SpeechErrorCode::BufferOverflow);
    }

    void liveSpeechPipeline()
    {
        const auto path = qgetenv("DESKWAIFU_TTS_LIVE_KEY_FILE");
        if (path.isEmpty() || !qEnvironmentVariableIsSet("DESKWAIFU_AUDIO_TEST"))
            QSKIP("Opt-in live API and audible playback test; requires both test environment variables");
        QFile file(QString::fromUtf8(path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString key;
        for (const auto& line : file.readAll().split('\n'))
            if (line.startsWith("API_KEY=")) key = QString::fromUtf8(line.mid(8).trimmed());
        QVERIFY(!key.isEmpty());
        MiniMaxSpeechSource source(key);
        AudioPlayer audio;
        SpeechPlayer player(source, audio);
        QSignalSpy errors(&player, &SpeechPlayer::ErrorOccurred);
        QSignalSpy done(&player, &SpeechPlayer::Finished);
        player.Speak({QStringLiteral("姐姐，你好喵"), {}});
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 60000);
        if (!errors.isEmpty()) qWarning() << qvariant_cast<SpeechError>(errors[0][1]).message;
        QCOMPARE(errors.size(), 0);
        QCOMPARE(qvariant_cast<SpeechPlayer::FinishReason>(done[0][1]), SpeechPlayer::FinishReason::Completed);
    }
};

QTEST_GUILESS_MAIN(AudioPlayerTest)
#include "AudioPlayerTest.moc"
