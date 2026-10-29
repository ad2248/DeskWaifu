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
    std::cout << "OCR client lifecycle and protocol tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
