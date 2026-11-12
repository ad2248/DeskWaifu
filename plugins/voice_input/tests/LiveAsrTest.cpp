#include "VoiceInputSettings.hpp"
#include "asr/NativeAsrClient.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QRegularExpression>
#include <QTimer>
#include <iostream>

// 不注册到普通 CTest；只在显式提供测试端点和凭据时访问云端。
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::cerr << "Usage: voice-input-live-test /path/to/synthetic.pcm\n";
        return 1;
    }
    VoiceInputConfig config;
    config.apiKey = qEnvironmentVariable("BAILIAN_API_KEY").trimmed();
    config.endpoint = qEnvironmentVariable("BAILIAN_ASR_ENDPOINT").trimmed();
    // Exercise the requested WebSocket model, independent of saved/default settings.
    config.model = QStringLiteral("qwen-audio-3.1-asr-flash-streaming");
    if (config.apiKey.isEmpty() || config.endpoint.isEmpty()) {
        std::cerr << "Live ASR requires explicit BAILIAN_API_KEY and BAILIAN_ASR_ENDPOINT\n";
        return 1;
    }
    if (!config.endpoint.startsWith(QStringLiteral("wss://"), Qt::CaseInsensitive)) {
        std::cerr << "Live ASR requires a secure WebSocket endpoint\n";
        return 1;
    }
    QFile fixture(QString::fromLocal8Bit(argv[1]));
    if (!fixture.open(QIODevice::ReadOnly) || fixture.size() < 32000 || fixture.size() > 320000 || fixture.size() % 2) {
        std::cerr << "Invalid synthetic PCM fixture\n";
        return 1;
    }
    const auto pcm = fixture.readAll();
    NativeAsrClient client;
    QEventLoop wait;
    bool success = false;
    QObject::connect(&client, &NativeAsrClient::Recognized, &wait, [&](quint64, const QString& text) {
        const auto words = text.toLower().split(QRegularExpression("[^a-z]+"), Qt::SkipEmptyParts);
        success = true;
        for (const auto* word : { "hello", "world", "voice", "input", "test" }) {
            if (!words.contains(QString::fromLatin1(word))) {
                success = false;
            }
        }
        if (!success) {
            std::cerr << "Live ASR did not recover the known synthetic phrase\n";
        }
        wait.quit();
    });
    QObject::connect(&client, &NativeAsrClient::Failed, &wait, [&](quint64, const QString& code, const QString& error) {
        // Client errors are fixed/redacted diagnostics, never remote response bodies.
        std::cerr << "Live ASR request failed: " << code.toStdString() << " / " << error.toStdString() << '\n';
        wait.quit();
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &wait, [&] {
        std::cerr << "Live ASR test deadline exceeded\n";
        client.Cancel();
        wait.quit();
    });
    deadline.start(125000);
    // Queue the start so even synchronous validation failures are observed by exec().
    QTimer::singleShot(0, &wait, [&] { client.Recognize(1, pcm, config); });
    wait.exec();
    if (success) {
        std::cout << "Live ASR production-client protocol and known synthetic phrase passed\n";
    }
    return success ? 0 : 1;
}
