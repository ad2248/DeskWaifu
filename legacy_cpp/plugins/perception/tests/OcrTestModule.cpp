#include "../private/processor/ocr/OcrBackend.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QThread>
#include <QUuid>
#include <stdexcept>

#ifdef OCR_TEST_MISSING_DEPENDENCY
extern "C" int OcrTestDependency();
#endif

class TestBackend : public OcrBackend {
public:
    QString mode = qEnvironmentVariable("DESKWAIFU_OCR_TEST_MODE");
    QString modePath = qEnvironmentVariable("DESKWAIFU_OCR_TEST_MODE_FILE");
    QString instance = QUuid::createUuid().toString();
    QThread* thread = QThread::currentThread();

    QJsonObject Initialize(const QString& detection, const QString& recognition) override
    {
        if (thread == QCoreApplication::instance()->thread()) {
            throw std::runtime_error("initialization on main thread");
        }
        if (mode == "arguments" && (detection != "/tmp/检测 模型" || recognition != "/tmp/识别 模型")) {
            throw std::runtime_error("incorrect model directories");
        }
        if (mode == "slow-load") {
            QThread::msleep(250);
        }
        if (mode == "load-error") {
            return { { "ok", false }, { "code", "model_incompatible" }, { "error", "model checksum mismatch" } };
        }
        if (mode == "throw-load") {
            throw std::runtime_error("test load exception");
        }
        return { { "ok", true } };
    }

    QJsonObject Recognize(const QString& image) override
    {
        if (thread != QThread::currentThread()) {
            throw std::runtime_error("inference thread changed");
        }
        if (!modePath.isEmpty()) {
            QFile file(modePath);
            if (!file.open(QIODevice::ReadOnly)) {
                throw std::runtime_error("mode file missing");
            }
            mode = QString::fromUtf8(file.readAll()).trimmed();
        }
        if (mode == "hang" || mode == "delayed-reply") {
            QThread::msleep(250);
        }
        if (!QFile::exists(image)) {
            throw std::runtime_error("input file released during inference");
        }
        if (mode == "throw") {
            throw std::runtime_error("test inference exception");
        }
        if (mode == "error") {
            return { { "ok", false }, { "error", "test request error" } };
        }
        QJsonObject result { { "width", 1 }, { "height", 1 }, { "lines", QJsonArray { } }, { "process_pid", QCoreApplication::applicationPid() }, { "instance", instance },
            { "background_thread", QThread::currentThread() != QCoreApplication::instance()->thread() } };
        if (mode == "text") {
            result.insert("lines",
                QJsonArray { QJsonObject { { "text", "synthetic clipboard text" }, { "confidence", 0.9 },
                    { "polygon", QJsonArray { QJsonArray { 0, 0 }, QJsonArray { 1, 0 }, QJsonArray { 1, 1 }, QJsonArray { 0, 1 } } } } });
        }
        if (mode == "bad-result") {
            result.insert("lines", QJsonArray { QJsonObject { { "text", "bad" } } });
        }
        return { { "ok", true }, { "result", result } };
    }

    void Release() noexcept override
    {
        if (thread != QThread::currentThread()) {
            qFatal("OCR release on wrong thread");
        }
        if (mode == "slow-release") {
            QThread::msleep(100);
        }
    }
};

class OcrTestModule : public QObject, public OcrBackendFactory {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DeskWaifuOcrBackend_iid)
    Q_INTERFACES(OcrBackendFactory)
public:
    OcrBackend* Create() override
    {
#ifdef OCR_TEST_MISSING_DEPENDENCY
        OcrTestDependency();
#endif
        return new TestBackend;
    }
};

#include "OcrTestModule.moc"
