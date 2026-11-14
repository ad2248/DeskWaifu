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
    void secureWebSocketEndpointAndReset()
    {
        QTemporaryDir dir;
        VoiceInputSettings settings(dir.path());
        QString error;
        QVERIFY(settings.Set("{\"endpoint\":\"wss://example.invalid/api-ws/v1/inference\"}", error));
        QCOMPARE(settings.Snapshot().endpoint, QString("wss://example.invalid/api-ws/v1/inference"));
        QVERIFY(!settings.Set("{\"endpoint\":\"ws://example.invalid/api-ws/v1/inference\"}", error));
        QVERIFY(!settings.Set("{\"endpoint\":\"wss://secret@example.invalid/asr\"}", error));
        QVERIFY(!settings.Set("{\"endpoint\":\"wss://example.invalid/asr#secret\"}", error));
        QVERIFY(settings.Set(R"({"endpoint":"","model":""})", error));
        QCOMPARE(settings.Snapshot().endpoint, VoiceInputConfig { }.endpoint);
        QCOMPARE(settings.Snapshot().model, VoiceInputConfig { }.model);
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
    void validationDoesNotLeakOrMutate()
    {
        QTemporaryDir dir;
        VoiceInputSettings settings(dir.path());
        QString error;
        QVERIFY(settings.Set(R"({"api_key":"retained-secret"})", error));
        for (const auto* json : { R"({"api_key":"private","api_key":"second"})", R"({"extension":{"x":1,"x":2}})", R"({"api_key":"private\nsecret"})", R"({"api_key":null})",
                 R"({"api_key":42})", R"({"max_recording_seconds":0})", R"({"max_recording_seconds":301})", R"({"max_recording_seconds":2.5})", R"({"max_recording_seconds":"60"})",
                 R"({"endpoint":"http://private.example"})", R"({"endpoint":"https://secret@private.example"})", R"({"endpoint":"https://private.example/#secret"})",
                 R"({"model":[]})", "[]", "{malformed private-secret}" }) {
            QVERIFY2(!settings.Set(json, error), json);
            QVERIFY(!error.isEmpty());
            QVERIFY(!error.contains("private"));
            QVERIFY(!error.contains("secret"));
            QCOMPARE(settings.Snapshot().apiKey, QString("retained-secret"));
        }
        QFile file(dir.filePath("config.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"api_key\":\"secret\",\"api_key\":\"other\"}");
        file.close();
        QVERIFY(!settings.Refresh(error));
        QCOMPARE(settings.Snapshot().apiKey, QString("retained-secret"));
    }
    void configStoreDetectsSnapshotConflict()
    {
        QTemporaryDir dir;
        ConfigStore store(dir.filePath("config.json").toStdString(), true);
        ConfigStore::Snapshot before, saved;
        std::string error;
        QVERIFY(store.Read(before, error));
        QVERIFY(store.Commit("{\"model\":\"first\"}", before, saved, error));
        QVERIFY(!store.Commit("{\"model\":\"second\"}", before, saved, error));
    }
};
QTEST_GUILESS_MAIN(SettingsTest)
#include "SettingsTest.moc"
