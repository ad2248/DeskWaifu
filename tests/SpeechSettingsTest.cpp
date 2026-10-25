#include "plugins/speech/SpeechSettings.hpp"
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <sys/stat.h>

namespace {
bool failTemporary = false;
QString concurrentPath;
QByteArray concurrentDocument;
bool concurrentWriteSucceeded = false;

bool WriteFile(const QString& path, const QByteArray& document)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(document) == document.size();
}

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

class SpeechSettingsTest : public QObject
{
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
        QVERIFY(settings.Set(QJsonObject{{"api_key", "test-quoted\"key"}}, error));
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
        QVERIFY(settings.Set(QJsonObject{{"api_key", ""}}, error));
        QVERIFY(!settings.Configured());
        QVERIFY(loaded.Refresh(error));
        QVERIFY(loaded.ApiKey().isEmpty());
        QVERIFY(store.Read(disk, diagnostic));
        QCOMPARE(disk.values.at("extension"), std::string(R"({"changed":true})"));
        QVERIFY(stat(QFile::encodeName(path).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
    }

    void rejectsInvalidSecretWithoutChangingRuntime()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.path() + "/config.json";
        SpeechSettings settings(directory.path());
        QString error = "stale error";
        QVERIFY(settings.Refresh(error));
        QVERIFY(error.isEmpty() && !settings.Configured());
        QVERIFY(!QFileInfo::exists(path));
        for (const auto& original : {QString(), QString("keep-runtime-key")}) {
            if (!original.isEmpty()) QVERIFY(settings.Set(QJsonObject{{"api_key", original}}, error));
            const auto document = ReadFile(path);
            for (const auto* text : {R"({"api_key":42})", R"({"api_key":null})", R"({"api_key":true})",
                R"({"api_key":[]})", R"({"api_key":{}})", R"({"api_key":"line\nbreak"})",
                R"({"api_key":"line\rbreak"})", R"({"api_key":"nul\u0000byte"})",
                R"({"api_key":"tab\tkey"})", R"({"api_key":"del\u007fkey"})", "{}", "[]",
                R"({"api_key":"first","api_key":"second"})", R"({"api_key":"first","\u0061pi_key":"second"})",
                R"({"api_key":"valid","extension":{"duplicate":1,"duplicate":2}})",
                R"({"api_key":"valid"} trailing)", "{ // comments are not IPC JSON\n\"api_key\":\"key\"}"}) {
                QVERIFY(!settings.Set(std::string(text), error));
                QVERIFY(!error.isEmpty());
                QCOMPARE(settings.ApiKey(), original);
                QCOMPARE(settings.Configured(), !original.isEmpty());
                QCOMPARE(ReadFile(path), document);
                if (original.isEmpty()) QVERIFY(!QFileInfo::exists(path));
            }
        }
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
        QVERIFY(settings.Set(QJsonObject{{"api_key", boundary}}, error));
        QVERIFY(QFileInfo::exists(path));
        const auto document = ReadFile(path);
        QVERIFY(!settings.Set(QJsonObject{{"api_key", boundary + 'x'}}, error));
        QCOMPARE(settings.ApiKey(), boundary);
        QCOMPARE(ReadFile(path), document);
        const QString unicode(2048, QChar(0xe9));
        QVERIFY(settings.Set(QJsonObject{{"api_key", unicode}}, error));
        QVERIFY(!settings.Set(QJsonObject{{"api_key", unicode + 'x'}}, error));
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
        QVERIFY(settings.Set(QJsonObject{{"api_key", "keep-runtime-key"}}, error));
        for (const auto* text : {"broken config", R"({"api_key":42})", R"({"api_key":"bad\nkey"})",
            R"({"api_key":"first","api_key":"second"})", R"({"extension":{"same":1,"same":2}})"}) {
            QVERIFY(WriteFile(path, text));
            QVERIFY(chmod(QFile::encodeName(path).constData(), 0644) == 0);
            QVERIFY(!settings.Refresh(error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(settings.ApiKey(), QString("keep-runtime-key"));
            QVERIFY(!settings.Set(QJsonObject{{"api_key", "not-saved"}}, error));
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
        QVERIFY(!settings.Set(QJsonObject{{"api_key", "not-saved"}}, error));
        QCOMPARE(settings.ApiKey(), QString("keep-runtime-key"));
        QVERIFY(QFileInfo(path).isDir());
    }

    void failedAndConflictingCommitsPreserveRuntimeAndDisk()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.path() + "/config.json";
        const auto restore = qScopeGuard([] { failTemporary = false; concurrentPath.clear(); });
        SpeechSettings settings(directory.path());
        QString error;
        QVERIFY(settings.Set(QJsonObject{{"api_key", "keep-runtime-key"}}, error));
        const auto document = ReadFile(path);
        failTemporary = true;
        QVERIFY(!settings.Set(QJsonObject{{"api_key", "not-saved"}}, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(settings.ApiKey(), QString("keep-runtime-key"));
        QCOMPARE(ReadFile(path), document);
        failTemporary = false;
        concurrentDocument = R"({"api_key":"external-key","extension":true})";
        concurrentWriteSucceeded = false;
        concurrentPath = path;
        QVERIFY(!settings.Set(QJsonObject{{"api_key", "not-saved"}}, error));
        QVERIFY(concurrentWriteSucceeded);
        QVERIFY(error.contains("changed"));
        QCOMPARE(settings.ApiKey(), QString("keep-runtime-key"));
        QCOMPARE(ReadFile(path), concurrentDocument);
        QVERIFY(QDir(directory.path()).entryList({".deskwaifu-config-*"}, QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY(settings.Set(QJsonObject{{"api_key", "retry-key"}}, error));
        QVERIFY(error.isEmpty());
        QCOMPARE(settings.ApiKey(), QString("retry-key"));
        ConfigStore::Snapshot disk;
        std::string diagnostic;
        QVERIFY(ConfigStore(path.toStdString()).Read(disk, diagnostic));
        QCOMPARE(disk.values.at("extension"), std::string("true"));
    }
};

extern "C" int __real_mkstemp(char*);
extern "C" int __wrap_mkstemp(char* pattern)
{
    if (failTemporary) { errno = EIO; return -1; }
    if (!concurrentPath.isEmpty()) {
        concurrentWriteSucceeded = WriteFile(concurrentPath, concurrentDocument);
        concurrentPath.clear();
    }
    return __real_mkstemp(pattern);
}

QTEST_GUILESS_MAIN(SpeechSettingsTest)
#include "SpeechSettingsTest.moc"
