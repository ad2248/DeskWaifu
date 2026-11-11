#include "private/audio/AudioCapture.hpp"
#include "private/audio/PcmNormalizer.hpp"
#include "private/audio/WavEncoder.hpp"
#include <QBuffer>
#include <QtEndian>
#include <QtTest>
#include <cmath>

class AudioTest : public QObject {
    Q_OBJECT
private:
    static QAudioFormat Format(int rate, int channels, QAudioFormat::SampleFormat sample)
    {
        QAudioFormat f;
        f.setSampleRate(rate);
        f.setChannelCount(channels);
        f.setSampleFormat(sample);
        return f;
    }
private slots:
    void passthroughAndTail()
    {
        PcmNormalizer normalizer;
        QString error;
        QByteArray result;
        QVERIFY(normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        const auto input = QByteArray::fromHex("0080ffff00000100ff7f");
        QVERIFY(normalizer.Append(input.first(3), error));
        QVERIFY(normalizer.Append(input.mid(3), error));
        QVERIFY(normalizer.Finish(result, error));
        QCOMPARE(result, input);
        QVERIFY(normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        QVERIFY(normalizer.Append(QByteArray(1, '\0'), error));
        QVERIFY(!normalizer.Finish(result, error));
    }
    void nativeFormats_data()
    {
        QTest::addColumn<int>("sample");
        QTest::addColumn<QByteArray>("input");
        const qint16 i16[2] = { 16384, 0 };
        const qint32 i32[2] = { 1073741824, 0 };
        const float f32[2] = { 0.5f, 0 };
        QTest::newRow("uint8") << int(QAudioFormat::UInt8) << QByteArray::fromHex("c080");
        QTest::newRow("int16") << int(QAudioFormat::Int16) << QByteArray(reinterpret_cast<const char*>(i16), sizeof(i16));
        QTest::newRow("int32") << int(QAudioFormat::Int32) << QByteArray(reinterpret_cast<const char*>(i32), sizeof(i32));
        QTest::newRow("float") << int(QAudioFormat::Float) << QByteArray(reinterpret_cast<const char*>(f32), sizeof(f32));
    }
    void nativeFormats()
    {
        QFETCH(int, sample);
        QFETCH(QByteArray, input);
        PcmNormalizer normalizer;
        QString error;
        QByteArray result;
        QVERIFY(normalizer.Reset(Format(16000, 2, QAudioFormat::SampleFormat(sample)), error));
        for (const char b : input) {
            QVERIFY(normalizer.Append(QByteArray(1, b), error));
        }
        QVERIFY(normalizer.Finish(result, error));
        QCOMPARE(result, QByteArray::fromHex("0020"));
    }
    void resamplingIsChunkInvariantAndFlushesTail()
    {
        QByteArray input;
        for (int i = 0; i < 48000; ++i) {
            const float sample = 0.4f * std::sin(i * 2.0 * 3.141592653589793 * 440 / 48000);
            input.append(reinterpret_cast<const char*>(&sample), 4);
            input.append(reinterpret_cast<const char*>(&sample), 4);
        }
        auto convert = [&](int chunk) {
            PcmNormalizer normalizer;
            QString error;
            QByteArray result;
            if (!normalizer.Reset(Format(48000, 2, QAudioFormat::Float), error)) {
                return QByteArray();
            }
            for (qsizetype i = 0; i < input.size(); i += chunk) {
                if (!normalizer.Append(input.mid(i, chunk), error)) {
                    return QByteArray();
                }
            }
            if (!normalizer.Finish(result, error)) {
                return QByteArray();
            }
            return result;
        };
        const auto large = convert(input.size());
        QCOMPARE(large.size(), 32000);
        QCOMPARE(convert(137), large);
        // The final samples must include the recorded signal, not only an implicit zero tail.
        QVERIFY(large.last(100) != QByteArray(100, '\0'));
    }
    void limitsAndInvalidFormat()
    {
        PcmNormalizer normalizer;
        QString error;
        QByteArray result;
        QVERIFY(!normalizer.Reset(QAudioFormat(), error));
        QVERIFY(normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        QVERIFY(!normalizer.Append(QByteArray(4 * 1024 * 1024 + 1, '\0'), error));
        normalizer.Clear();
        QVERIFY(!normalizer.Append({ }, error));
        QVERIFY(normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        const QByteArray second(32000, '\0');
        for (int i = 0; i < 300; ++i) {
            QVERIFY(normalizer.Append(second, error));
        }
        QVERIFY(!normalizer.Append(QByteArray(2, '\0'), error));
    }
    void stopDrainsUnreadPullDeviceAndRejectsStaleSession()
    {
        AudioCapture capture;
        QString error;
        QByteArray result;
        const auto tail = QByteArray::fromHex("010002000300");
        QBuffer device;
        device.setData(tail);
        QVERIFY(device.open(QIODevice::ReadOnly));
        QVERIFY(capture._normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        QVERIFY(capture._normalizer.Append(QByteArray::fromHex("0080"), error));
        capture._active = true;
        capture._sessionId = 9;
        capture._device = &device;
        capture._maxSeconds = 60;
        capture._maxInputBytes = 60 * 32000;
        QVERIFY(!capture.Stop(8, result, error));
        QCOMPARE(device.pos(), qint64(0));
        QVERIFY(capture.Stop(9, result, error));
        QCOMPARE(result, QByteArray::fromHex("0080") + tail);
        QVERIFY(!capture.Stop(9, result, error));
        QCOMPARE(capture._device, nullptr);
    }
    void captureDurationBudgetTruncates_data()
    {
        QTest::addColumn<int>("seconds");
        QTest::addColumn<int>("extraFrames");
        QTest::addColumn<int>("channels");
        QTest::addColumn<bool>("splitFrame");
        QTest::newRow("one-second-plus-one-frame") << 1 << 1 << 1 << false;
        QTest::newRow("large-crossing-block") << 1 << 100000 << 1 << false;
        QTest::newRow("split-native-stereo-frame") << 1 << 100000 << 2 << true;
        QTest::newRow("maximum-300-seconds") << 300 << 1 << 1 << false;
    }
    void captureDurationBudgetTruncates()
    {
        QFETCH(int, seconds);
        QFETCH(int, extraFrames);
        QFETCH(int, channels);
        QFETCH(bool, splitFrame);
        AudioCapture capture;
        QString error;
        QByteArray result, expected, native;
        QSignalSpy failed(&capture, &AudioCapture::Failed);
        const int frames = seconds * 16000;
        for (int i = 0; i < frames; ++i) {
            const auto sample = qToLittleEndian<qint16>(qint16(i % 30000 - 15000));
            const QByteArray bytes(reinterpret_cast<const char*>(&sample), 2);
            expected += bytes;
            for (int c = 0; c < channels; ++c) {
                native += bytes;
            }
        }
        const auto budget = native.size();
        native += QByteArray(extraFrames * channels * 2, '\x7f');
        QBuffer device;
        device.setData(native);
        QVERIFY(device.open(QIODevice::ReadOnly));
        QVERIFY(capture._normalizer.Reset(Format(16000, channels, QAudioFormat::Int16), error));
        capture._active = true;
        capture._sessionId = 4;
        capture._device = &device;
        capture._maxSeconds = seconds;
        capture._maxInputBytes = budget;
        if (splitFrame) {
            const auto prefix = device.read(budget - 1);
            QVERIFY(capture._normalizer.Append(prefix, error));
            capture._inputBytes = prefix.size();
        }
        capture.OnAudioReady();
        QVERIFY(capture._budgetReached);
        QVERIFY(capture._pendingFailure.isEmpty());
        QCOMPARE(capture._inputBytes, qint64(budget));
        QCOMPARE(device.pos(), qint64(native.size()));
        // A further readyRead and the normal controller Stop must remain successful.
        capture.OnAudioReady();
        QVERIFY(capture.Stop(4, result, error));
        QVERIFY(error.isEmpty());
        QCOMPARE(result, expected);
        QCOMPARE(result.size(), seconds * 32000);
        QCOMPARE(failed.size(), 0);
        capture.Cancel();
        QVERIFY(!capture._budgetReached);
        QVERIFY(!capture._normalizer.Finish(result, error));
    }
    void budgetReachedStillHasIndependentWatchdog()
    {
        AudioCapture capture;
        QString error;
        QSignalSpy failed(&capture, &AudioCapture::Failed);
        QBuffer device;
        device.setData(QByteArray(32002, '\0'));
        QVERIFY(device.open(QIODevice::ReadOnly));
        QVERIFY(capture._normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        capture._active = true;
        capture._sessionId = 15;
        capture._device = &device;
        capture._maxSeconds = 1;
        capture._maxInputBytes = 32000;
        capture._limit.start(2000);
        capture.OnAudioReady();
        QVERIFY(capture._budgetReached);
        QVERIFY(capture._limit.isActive());
        // If the controller never stops, the independent watchdog still terminates capture.
        QVERIFY(QMetaObject::invokeMethod(&capture._limit, "timeout", Qt::DirectConnection));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toULongLong(), quint64(15));
        QVERIFY(!capture._active);
        QCOMPARE(capture._device, nullptr);
    }
    void deviceSchemaUsesGuiDefaultKey()
    {
        for (const bool isDefault : { false, true }) {
            const auto device = AudioCapture::DeviceInfo("offline-device", "Microphone", isDefault);
            QCOMPARE(device.value("id").toString(), QString::fromLatin1(QByteArray("offline-device").toBase64(QByteArray::Base64UrlEncoding)));
            QCOMPARE(device.value("name").toString(), QString("Microphone"));
            QVERIFY(device.value("is_default").isBool());
            QCOMPARE(device.value("is_default").toBool(), isDefault);
            QVERIFY(!device.contains("default"));
        }
    }
    void readyReadFailureDefersCleanupAndBlocksStop()
    {
        AudioCapture capture;
        QString error;
        QByteArray pcm;
        QSignalSpy failed(&capture, &AudioCapture::Failed);
        QBuffer device;
        device.setData(QByteArray(20, '\0'));
        QVERIFY(device.open(QIODevice::ReadOnly));
        QVERIFY(capture._normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        // A genuine conversion failure must still defer teardown and reject Stop.
        capture._normalizer.Clear();
        capture._active = true;
        capture._sessionId = 11;
        capture._device = &device;
        capture._maxSeconds = 1;
        capture._maxInputBytes = 32000;
        connect(&device, &QIODevice::readyRead, &capture, &AudioCapture::OnAudioReady);
        // Emit synchronously: teardown and Failed must not occur inside the device's signal stack.
        QVERIFY(QMetaObject::invokeMethod(&device, "readyRead", Qt::DirectConnection));
        QCOMPARE(capture._device, &device);
        QCOMPARE(failed.size(), 0);
        QVERIFY(!capture._pendingFailure.isEmpty());
        QVERIFY(!capture.Stop(11, pcm, error));
        QVERIFY(pcm.isEmpty());
        QCOMPARE(capture._device, &device);
        QVERIFY(QMetaObject::invokeMethod(&device, "readyRead", Qt::DirectConnection));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toULongLong(), quint64(11));
        QCOMPARE(failed[0][1].toString(), QString("audio_limit"));
        QCOMPARE(capture._device, nullptr);
        QVERIFY(!capture._active);
    }
    void cancelledQueuedFailureDoesNotAffectReplacementCapture()
    {
        AudioCapture capture;
        QString error;
        QByteArray pcm;
        QSignalSpy failed(&capture, &AudioCapture::Failed);
        capture._active = true;
        capture._sessionId = 12;
        capture.QueueFailure("audio_device", "Audio input device failed");
        capture.Cancel();
        // Reuse the id deliberately: the internal generation must protect even this case.
        QBuffer device;
        device.setData(QByteArray::fromHex("01000200"));
        QVERIFY(device.open(QIODevice::ReadOnly));
        QVERIFY(capture._normalizer.Reset(Format(16000, 1, QAudioFormat::Int16), error));
        capture._active = true;
        capture._sessionId = 12;
        capture._device = &device;
        capture._maxSeconds = 1;
        capture._maxInputBytes = 32000;
        QCoreApplication::sendPostedEvents(&capture, QEvent::MetaCall);
        QCOMPARE(failed.size(), 0);
        QVERIFY(capture._active);
        QVERIFY(capture.Stop(12, pcm, error));
        QCOMPARE(pcm, QByteArray::fromHex("01000200"));
    }
    void wavBytes()
    {
        QString error;
        const auto pcm = QByteArray::fromHex("0080ff7f0100");
        const auto wav = WavEncoder::Encode(pcm, error);
        QCOMPARE(wav.size(), 44 + pcm.size() + 8000);
        QCOMPARE(wav.first(4), QByteArray("RIFF"));
        QCOMPARE(wav.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 4), quint32(wav.size() - 8));
        QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 20), quint16(1));
        QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 22), quint16(1));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 24), quint32(16000));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 28), quint32(32000));
        QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 32), quint16(2));
        QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 34), quint16(16));
        QCOMPARE(wav.mid(36, 4), QByteArray("data"));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 40), quint32(pcm.size() + 8000));
        QCOMPARE(wav.mid(44, pcm.size()), pcm);
        QCOMPARE(wav.last(8000), QByteArray(8000, '\0'));
        QVERIFY(WavEncoder::Encode(QByteArray(1, '\0'), error).isEmpty());
    }
};
QTEST_GUILESS_MAIN(AudioTest)
#include "AudioTest.moc"
