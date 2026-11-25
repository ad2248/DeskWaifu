#include "VoiceInputSettings.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>

class SettingsTest : public QObject {
    Q_OBJECT
private slots:
    void defaultsPartialUpdatesPrivacyAndClear()
    {
        QTemporaryDir dir;
        VoiceInputSettings settings(dir.path());
        QString error;
        QVERIFY(settings.Refresh(error));
        QVERIFY(!settings.Configured());
        QCOMPARE(settings.Snapshot().maxRecordingSeconds, 60);
        QCOMPARE(settings.Snapshot().endpoint, QString("wss://llm-y3exskfcgxgxzn23.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference"));
        QCOMPARE(settings.Snapshot().model, QString("qwen-audio-3.1-asr-flash-streaming"));
        QVERIFY(settings.Set(R"({"api_key":"secret","device_id":"mic","extension":{"n":9007199254740993}})", error));
        const auto old = settings.Snapshot();
        QVERIFY(settings.Set(R"({"model":"custom","max_recording_seconds":300})", error));
        QCOMPARE(settings.Snapshot().apiKey, QString("secret"));
        QCOMPARE(old.model, QString("qwen-audio-3.1-asr-flash-streaming"));
        QVERIFY(!settings.PublicView().contains("api_key"));
        QVERIFY(!QJsonDocument(settings.PublicView()).toJson().contains("secret"));
        struct stat info;
        QVERIFY(stat(QFile::encodeName(dir.filePath("config.json")).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
        ConfigStore store(dir.filePath("config.json").toStdString());
        ConfigStore::Snapshot disk;
        std::string diagnostic;
        QVERIFY(store.Read(disk, diagnostic));
        QCOMPARE(disk.values.at("extension"), std::string(R"({"n":9007199254740993})"));
        QVERIFY(settings.Set(R"({"api_key":"","device_id":""})", error));
        QVERIFY(!settings.Configured());
        VoiceInputSettings reloaded(dir.path());
        QVERIFY(reloaded.Refresh(error));
        QVERIFY(!reloaded.Configured());
        QCOMPARE(reloaded.Snapshot().maxRecordingSeconds, 300);
    }
    void ipcCommandIsNotPersistedAndUnknownFieldsRemain()
    {
        QTemporaryDir dir;
        VoiceInputSettings settings(dir.path());
        QString error;
        QVERIFY(settings.Set(R"({"extension":{"n":9007199254740993},"future_setting":"keep"})", error));
        QVERIFY(settings.Set(R"({"command":"set_voice_input_settings","api_key":"secret","another_extension":true})", error));
        ConfigStore store(dir.filePath("config.json").toStdString());
        ConfigStore::Snapshot disk;
        std::string diagnostic;
        QVERIFY(store.Read(disk, diagnostic));
        QVERIFY(disk.values.find("command") == disk.values.end());
        QCOMPARE(disk.values.at("extension"), std::string(R"({"n":9007199254740993})"));
        QCOMPARE(disk.values.at("future_setting"), std::string(R"("keep")"));
        QCOMPARE(disk.values.at("another_extension"), std::string("true"));
        QCOMPARE(settings.Snapshot().apiKey, QString("secret"));
    }
    void corruptFileDoesNotReplaceRuntimeSettings()
    {
        QTemporaryDir dir;
        VoiceInputSettings settings(dir.path());
        QString error;
        QVERIFY(settings.Set(R"({"api_key":"retained-secret"})", error));
        QFile file(dir.filePath("config.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"api_key\":\"secret\",\"api_key\":\"other\"}");
        file.close();
        QVERIFY(!settings.Refresh(error));
        QCOMPARE(settings.Snapshot().apiKey, QString("retained-secret"));
    }
};
QTEST_GUILESS_MAIN(SettingsTest)
#include "SettingsTest.moc"
