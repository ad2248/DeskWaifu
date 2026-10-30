#include "processor/ocr/OcrClient.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void Wait(const std::function<bool()>& predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    Check(predicate(), "event-loop wait timed out");
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "expected helper path");
    const QString helper = app.arguments()[1];
    QTemporaryDir directory;
    const auto image = directory.filePath("image.png");
    QFile file(image);
    Check(file.open(QIODevice::WriteOnly), "temporary image"); file.close();
    {
        OcrClient absent(directory.filePath("missing"));
        Check(!absent.Status().value("available").toBool(), "optional package must be absent");
        Check(!absent.Submit(image).value("ok").toBool(), "missing package must fail cleanly");
        Check(!absent.Status().value("running").toBool(), "missing package must not start process");
    }
    {
        const auto broken = directory.filePath("broken-helper");
        QFile executable(broken);
        Check(executable.open(QIODevice::WriteOnly), "temporary executable");
        executable.write("#!/nonexistent/ocr-interpreter\n"); executable.close();
        Check(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "executable permissions");
        OcrClient client(broken, 1000, 100);
        const auto id = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(id).value("state") == "failed"; });
        Check(!client.Status().value("running").toBool(), "failed-to-start must release worker");
    }
    {
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
        OcrClient client(helper, 1000, 100);
        Check(!client.Submit("relative.png").value("ok").toBool(), "reject relative path");
        Check(!client.Status().value("running").toBool(), "lazy startup");
        const auto first = client.Submit(image).value("job_id").toString();
        Check(!first.isEmpty(), "request accepted");
        Check(!client.Submit(image).value("ok").toBool(), "reject busy request without queue");
        Wait([&] { return client.Result(first).value("state") == "completed"; });
        const auto pid = client.Result(first).value("response").toObject().value("result").toObject().value("helper_pid");
        const auto second = client.Submit(image).value("job_id").toString();
        Check(second != first, "unique job ids");
        Check(!client.Result(first).value("ok").toBool(), "bounded receipt retention");
        Check(!client.Cancel(first).value("ok").toBool(), "stale cancel cannot cancel new request");
        Wait([&] { return client.Result(second).value("state") == "completed"; });
        Check(pid == client.Result(second).value("response").toObject().value("result").toObject().value("helper_pid"), "reuse loaded worker");
        Wait([&] { return !client.Status().value("running").toBool(); });
        const auto third = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(third).value("state") == "completed"; });
        Check(pid != client.Result(third).value("response").toObject().value("result").toObject().value("helper_pid"), "restart after idle exit");
    }
    for (const auto* mode : {"no-ready", "load-error", "version", "crash", "hang", "oversized", "garbage", "bad-id", "bad-result", "error"}) {
        qputenv("DESKWAIFU_OCR_TEST_MODE", mode);
        OcrClient client(helper, 150, 100);
        const auto id = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(id).value("state") == "failed"; });
        Check(!client.Result(id).value("response").toObject().value("error").toString().isEmpty(), "failure diagnostic");
        if (QString::fromLatin1(mode) == "load-error")
            Check(client.Result(id).value("response").toObject().value("error").toString().contains("model checksum mismatch"), "preserve initialization error");
        Wait([&] { return !client.Status().value("running").toBool(); });
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
        const auto retry = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(retry).value("state") == "completed"; });
    }
    {
        qputenv("DESKWAIFU_OCR_TEST_MODE", "hang");
        OcrClient client(helper, 1000, 100);
        const auto id = client.Submit(image).value("job_id").toString();
        Check(client.Cancel(id).value("ok").toBool(), "cancel accepted");
        Check(client.Result(id).value("state") == "failed", "cancel records failure");
        Wait([&] { return !client.Status().value("running").toBool(); });
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
        const auto retry = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(retry).value("state") == "completed"; });
    }
    {
        OcrClient client(directory.filePath("missing"));
        client.ConfigureModels({});
        Check(client.Status().value("error_code") == "helper_missing", "missing helper diagnostic");
        Check(client.Status().value("detection_model_dir") == "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det", "default detection directory");
        Check(client.Status().value("recognition_model_dir") == "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec", "default recognition directory");
    }
    for (const QJsonValue value : {QJsonValue("relative"), QJsonValue(""), QJsonValue(42), QJsonValue(QJsonValue::Null)}) {
        OcrClient client(helper);
        client.ConfigureModels({{"ocr_detection_model_dir", value}});
        Check(client.Status().value("error_code") == "invalid_config", "invalid configured directory");
        Check(!client.Status().value("running").toBool(), "invalid config must not start worker");
        Check(!client.Submit(image).value("ok").toBool(), "invalid config rejects task");
        Check(client.Status().value("job_id").toString().isEmpty(), "rejection creates no job");
    }
    {
        OcrClient client(helper);
        client.ConfigureModels({}, "Malformed configuration JSON");
        Check(client.Status().value("error_code") == "invalid_config", "configuration read error disables OCR");
    }
    {
        qputenv("DESKWAIFU_OCR_TEST_MODE", "arguments");
        OcrClient client(helper, 1000, 100);
        client.ConfigureModels({{"ocr_detection_model_dir", "/tmp/检测 模型"}, {"ocr_recognition_model_dir", "/tmp/识别 模型"}});
        Check(client.Status().value("availability_state") == "checking", "asynchronous startup validation");
        Check(!client.Submit(image).value("ok").toBool(), "checking rejects tasks");
        Wait([&] { return client.Status().value("available").toBool(); });
        Check(client.Status().value("state") == "idle", "validation is not a recognition task");
        const auto id = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(id).value("state") == "completed"; });
        Wait([&] { return !client.Status().value("running").toBool(); });
        Check(client.Status().value("available").toBool(), "idle exit retains last validated availability");
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    }
    {
        qputenv("DESKWAIFU_OCR_TEST_MODE", "delayed-reply");
        OcrClient client(helper, 1000, 1000);
        client.ConfigureModels({});
        Wait([&] { return client.Status().value("available").toBool(); });
        const auto id = client.Submit(image).value("job_id").toString();
        QCoreApplication::processEvents(); // Flush the request without consuming the delayed reply.
        QThread::msleep(150);
        Check(client.Cancel(id).value("ok").toBool(), "cancel with reply already buffered");
        Wait([&] { return !client.Status().value("running").toBool(); });
        Check(client.Status().value("available").toBool(), "retired worker output must not disable OCR");
        const auto retry = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(retry).value("state") == "completed"; });
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    }
    {
        const auto modePath = directory.filePath("worker-mode");
        QFile modeFile(modePath);
        Check(modeFile.open(QIODevice::WriteOnly), "create worker mode file");
        modeFile.write("crash-request"); modeFile.close();
        qputenv("DESKWAIFU_OCR_TEST_MODE_FILE", modePath.toUtf8());
        OcrClient client(helper, 1000, 100);
        client.ConfigureModels({});
        Wait([&] { return client.Status().value("available").toBool(); });
        const auto id = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(id).value("state") == "failed" && !client.Status().value("running").toBool(); });
        Check(!client.Status().value("available").toBool(), "worker crash invalidates availability");
        Check(modeFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "update worker mode file");
        modeFile.write("text"); modeFile.close();
        const auto retry = client.Submit(image).value("job_id").toString();
        Check(!retry.isEmpty(), "explicit request can recover previously validated worker");
        Wait([&] { return client.Result(retry).value("state") == "completed"; });
        Wait([&] { return !client.Status().value("running").toBool(); });
        qputenv("DESKWAIFU_OCR_TEST_MODE", "load-error");
        const auto failedReload = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(failedReload).value("state") == "failed"; });
        Check(!client.Submit(image).value("ok").toBool(), "model load failure stays disabled even after previous success");
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
        qunsetenv("DESKWAIFU_OCR_TEST_MODE_FILE");
    }
    for (const auto* mode : {"load-error", "no-ready", "version", "crash"}) {
        qputenv("DESKWAIFU_OCR_TEST_MODE", mode);
        OcrClient client(helper, 150, 100);
        client.ConfigureModels({});
        Wait([&] { return client.Status().value("availability_state") == "unavailable"; });
        Check(!client.Status().value("error").toString().isEmpty(), "startup failure has diagnostic");
        Wait([&] { return !client.Status().value("running").toBool(); });
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
        if (QString::fromLatin1(mode) == "crash") {
            const auto retry = client.Submit(image).value("job_id").toString();
            Wait([&] { return client.Result(retry).value("state") == "completed"; });
        } else Check(!client.Submit(image).value("ok").toBool(), "initialization failure rejects tasks without retry loop");
    }
    std::cout << "OCR client lifecycle and protocol tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
