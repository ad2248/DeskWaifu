#include "SpeechSettings.hpp"
#include "MouseLookSettings.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>

class SpeechSettingsTest : public QObject
{
    Q_OBJECT
private slots:
    void persistsSecretPrivatelyAndOtherSettingsPreserveIt()
    {
        QTemporaryDir directory;
        LAppConfig config;
        config.sourcePath = (directory.path() + "/config.json").toStdString();
        QFile file(QString::fromStdString(config.sourcePath));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"model_scale\":2}");
        file.close();
        QVERIFY(chmod(config.sourcePath.c_str(), 0644) == 0);
        SpeechSettings settings(config);
        ConfigStore::Snapshot request;
        std::string error;
        QVERIFY(ConfigStore::Parse("{\"api_key\":\"test-quoted\\\"key\"}", request, error));
        QVERIFY(settings.Set(request).ok);
        QVERIFY(settings.Configured());
        QCOMPARE(config.values.modelScale, 2.0f);
        QCOMPARE(config.values.minimaxApiKey, std::string("test-quoted\"key"));
        struct stat info;
        QVERIFY(stat(config.sourcePath.c_str(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
        MouseLookSettings look(config);
        QVERIFY(look.Set(std::string(R"({"radius":0.75})")).ok);
        LAppConfig loaded;
        QVERIFY(loaded.LoadFromFile(config.sourcePath));
        QCOMPARE(loaded.values.minimaxApiKey, config.values.minimaxApiKey);
        QCOMPARE(loaded.values.mouseLookRadius, 0.75f);
        QVERIFY(ConfigStore::Parse(R"({"api_key":""})", request, error));
        QVERIFY(settings.Set(request).ok);
        QVERIFY(!settings.Configured());
        QVERIFY(loaded.LoadFromFile(config.sourcePath));
        QVERIFY(loaded.values.minimaxApiKey.empty());
    }

    void rejectsInvalidSecretWithoutChangingRuntime()
    {
        QTemporaryDir directory;
        LAppConfig config;
        config.sourcePath = (directory.path() + "/config.json").toStdString();
        SpeechSettings settings(config);
        for (const auto text : {R"({"api_key":42})", R"({"api_key":"line\nbreak"})", R"({"api_key":"nul\u0000byte"})", "{}"}) {
            ConfigStore::Snapshot request;
            std::string error;
            QVERIFY(ConfigStore::Parse(text, request, error));
            QVERIFY(!settings.Set(request).ok);
            QVERIFY(!settings.Configured());
        }
        QFile file(QString::fromStdString(config.sourcePath));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("broken config");
        file.close();
        ConfigStore::Snapshot request;
        std::string error;
        QVERIFY(ConfigStore::Parse(R"({"api_key":"not-saved"})", request, error));
        QVERIFY(!settings.Set(request).ok);
        QVERIFY(!settings.Configured());
    }
};

QTEST_GUILESS_MAIN(SpeechSettingsTest)
#include "SpeechSettingsTest.moc"
