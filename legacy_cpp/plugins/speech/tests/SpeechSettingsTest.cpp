#include "SpeechSettings.hpp"
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>

namespace {
bool WriteFile(const QString& path, const QByteArray& document)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(document) == document.size();
}

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray { };
}
}

class SpeechSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void persistsSecretPrivatelyAndOtherSettingsPreserveIt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.path() + "/config.json";
        QVERIFY(WriteFile(path, "{ // plugin configuration\n\"extension\":{\"number\":9007199254740993}}"));
        QVERIFY(chmod(QFile::encodeName(path).constData(), 0644) == 0);
        SpeechSettings settings(directory.path());
        QString error = "stale error";
        QVERIFY(settings.Set(QJsonObject { { "api_key", "test-quoted\"key" } }, error));
        QVERIFY(error.isEmpty());
        QVERIFY(settings.Configured());
        QCOMPARE(settings.ApiKey(), QString("test-quoted\"key"));
        struct stat info;
        QVERIFY(stat(QFile::encodeName(path).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
        ConfigStore store(path.toStdString());
        ConfigStore::Snapshot disk, saved;
        std::string diagnostic;
        QVERIFY(store.Read(disk, diagnostic));
        QCOMPARE(disk.values.at("extension"), std::string(R"({"number":9007199254740993})"));
        // 插件的其他配置写入者保留凭据，语音设置更新也保留未知字段的原始 JSON。
        const std::string externalDocument = "{\"api_key\":\"test-quoted\\\"key\",\"extension\":{\"changed\":true}}";
        QVERIFY(store.Commit(externalDocument, disk, saved, diagnostic));
        SpeechSettings loaded(directory.path());
        QVERIFY(loaded.Refresh(error));
        QCOMPARE(loaded.ApiKey(), settings.ApiKey());
        QVERIFY(chmod(QFile::encodeName(path).constData(), 0644) == 0);
        QVERIFY(settings.Set(QJsonObject { { "api_key", "" } }, error));
        QVERIFY(!settings.Configured());
        QVERIFY(loaded.Refresh(error));
        QVERIFY(loaded.ApiKey().isEmpty());
        QVERIFY(store.Read(disk, diagnostic));
        QCOMPARE(disk.values.at("extension"), std::string(R"({"changed":true})"));
        QVERIFY(stat(QFile::encodeName(path).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
    }

    void enforcesByteLimitAndCreatesPluginDirectory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto pluginDirectory = directory.path() + "/plugin/speech";
        const auto path = pluginDirectory + "/config.json";
        SpeechSettings settings(pluginDirectory);
        QString error;
        const QString boundary(4096, 'x');
        QVERIFY(settings.Set(QJsonObject { { "api_key", boundary } }, error));
        QVERIFY(QFileInfo::exists(path));
        const auto document = ReadFile(path);
        QVERIFY(!settings.Set(QJsonObject { { "api_key", boundary + 'x' } }, error));
        QCOMPARE(settings.ApiKey(), boundary);
        QCOMPARE(ReadFile(path), document);
        const QString unicode(2048, QChar(0xe9));
        QVERIFY(settings.Set(QJsonObject { { "api_key", unicode } }, error));
        QVERIFY(!settings.Set(QJsonObject { { "api_key", unicode + 'x' } }, error));
        QCOMPARE(settings.ApiKey(), unicode);
        SpeechSettings loaded(pluginDirectory);
        QVERIFY(loaded.Refresh(error));
        QCOMPARE(loaded.ApiKey(), unicode);
        struct stat info;
        QVERIFY(stat(QFile::encodeName(path).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
    }

    void badFilesAreNeverOverwrittenOrApplied()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.path() + "/config.json";
        SpeechSettings settings(directory.path());
        QString error;
        QVERIFY(settings.Set(QJsonObject { { "api_key", "keep-runtime-key" } }, error));
        for (const auto* text :
            { "broken config", R"({"api_key":42})", R"({"api_key":"bad\nkey"})", R"({"api_key":"first","api_key":"second"})", R"({"extension":{"same":1,"same":2}})" }) {
            QVERIFY(WriteFile(path, text));
            QVERIFY(chmod(QFile::encodeName(path).constData(), 0644) == 0);
            QVERIFY(!settings.Refresh(error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(settings.ApiKey(), QString("keep-runtime-key"));
            QVERIFY(!settings.Set(QJsonObject { { "api_key", "not-saved" } }, error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(settings.ApiKey(), QString("keep-runtime-key"));
            QCOMPARE(ReadFile(path), QByteArray(text));
            struct stat info;
            QVERIFY(stat(QFile::encodeName(path).constData(), &info) == 0);
            QCOMPARE(info.st_mode & 0777, mode_t(0644));
        }
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkdir(path));
        QVERIFY(!settings.Refresh(error));
        QVERIFY(!settings.Set(QJsonObject { { "api_key", "not-saved" } }, error));
        QCOMPARE(settings.ApiKey(), QString("keep-runtime-key"));
        QVERIFY(QFileInfo(path).isDir());
    }

};

QTEST_GUILESS_MAIN(SpeechSettingsTest)
#include "SpeechSettingsTest.moc"
