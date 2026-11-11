#include "processor/ocr/OcrClient.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
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

QJsonObject Recognize(OcrClient& client, const QString& image)
{
    const auto id = client.Submit(image).value("job_id").toString();
    Check(!id.isEmpty(), "request accepted");
    Wait([&] { return client.Result(id).value("state") == "completed"; });
    return client.Result(id).value("response").toObject().value("result").toObject();
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "expected module path");
    const QString module = app.arguments()[1];
    QTemporaryDir directory;
    const auto image = directory.filePath("image.png");
    QFile file(image);
    Check(file.open(QIODevice::WriteOnly), "temporary image");
    file.close();
    {
        qputenv("DESKWAIFU_OCR_HELPER", "/obsolete-helper-must-be-ignored");
        qputenv("DESKWAIFU_OCR_MODULE", module.toUtf8());
        Check(OcrClient::DefaultModule() == module, "only the module override is supported");
        qunsetenv("DESKWAIFU_OCR_HELPER");
        qunsetenv("DESKWAIFU_OCR_MODULE");
        OcrClient absent(directory.filePath("missing"));
        absent.ConfigureModels({ });
        Check(absent.Status().value("error_code") == "module_missing", "missing optional module diagnostic");
        Check(!absent.Submit(image).value("ok").toBool(), "missing package fails cleanly");
        Check(!absent.Status().value("running").toBool(), "missing package does not load engine");
    }
    {
        OcrClient broken(image, 1000, 100);
        broken.ConfigureModels({ });
        Wait([&] { return broken.Status().value("availability_state") == "unavailable"; });
        Check(broken.Status().value("error_code") == "module_incompatible", "invalid module diagnostic");
        Wait([&] { return !broken.Status().value("running").toBool(); });
    }
    for (const auto* path : { OCR_MISSING_DEPENDENCY_MODULE, OCR_INCOMPATIBLE_MODULE }) {
        OcrClient client(QString::fromUtf8(path), 1000, 30);
        client.ConfigureModels({ });
        Wait([&] { return client.Status().value("availability_state") == "unavailable"; });
        const auto expected = QString::fromUtf8(path) == OCR_MISSING_DEPENDENCY_MODULE ? "module_load_failed" : "module_incompatible";
        Check(client.Status().value("error_code") == expected, "dependency/ABI mismatch disables only OCR");
        Wait([&] { return !client.Status().value("running").toBool(); });
    }
    {
        OcrClient client(module, 1000, 100);
        Check(!client.Status().value("available").toBool(), "unvalidated module is not available yet");
        Check(!client.Submit("relative.png").value("ok").toBool(), "reject relative path");
        Check(!client.Status().value("running").toBool(), "lazy engine startup");
        const auto first = client.Submit(image).value("job_id").toString();
        Check(!first.isEmpty(), "request accepted");
        Check(!client.Submit(image).value("ok").toBool(), "reject busy request without queue");
        Wait([&] { return client.Result(first).value("state") == "completed"; });
        const auto result = client.Result(first).value("response").toObject().value("result").toObject();
        Check(result.value("process_pid").toInteger() == app.applicationPid(), "inference runs inside host process");
        Check(result.value("background_thread").toBool(), "inference is off main thread");
        const auto second = Recognize(client, image);
        Check(!client.Result(first).value("ok").toBool(), "bounded receipt retention");
        Check(!client.Cancel(first).value("ok").toBool(), "stale cancellation rejected");
        Check(second.value("instance") == result.value("instance"), "reuse loaded model instance");
        Wait([&] { return !client.Status().value("running").toBool(); });
        Check(Recognize(client, image).value("instance") != result.value("instance"), "reinitialize after idle release");
    }
    for (const auto* mode : { "error", "throw", "bad-result", "load-error", "throw-load" }) {
        qputenv("DESKWAIFU_OCR_TEST_MODE", mode);
        OcrClient client(module, 1000, 30);
        const auto id = client.Submit(image).value("job_id").toString();
        Wait([&] { return client.Result(id).value("state") == "failed"; });
        Check(!client.Result(id).value("response").toObject().value("error").toString().isEmpty(), "failure diagnostic");
        if (QString::fromLatin1(mode) == "load-error") {
            Check(client.Result(id).value("response").toObject().value("error").toString().contains("model checksum mismatch"), "preserve initialization error");
        }
        Wait([&] { return !client.Status().value("running").toBool(); });
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
        Recognize(client, image);
    }
    // Cancel/timeout must finish the user task promptly, but keep the worker occupied
    // until its real completion. Buffered results must never resurrect the task.
    for (const auto* mode : { "hang", "slow-load", "delayed-reply" }) {
        for (bool cancel : { true, false }) {
            qputenv("DESKWAIFU_OCR_TEST_MODE", mode);
            OcrClient client(module, cancel ? 1000 : 50, 30);
            int completions = 0;
            QObject::connect(&client, &OcrClient::Finished, &app, [&](const QString&, const QJsonObject&) { ++completions; });
            const auto id = client.Submit(image).value("job_id").toString();
            // Give a warm-engine inference time to enter its blocking call.
            QElapsedTimer elapsed;
            elapsed.start();
            while (elapsed.elapsed() < 20) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            if (cancel) {
                Check(client.Cancel(id).value("ok").toBool(), "cancel accepted");
            }
            Wait([&] { return client.Result(id).value("state") == "failed"; });
            Check(client.Status().value("busy").toBool() && client.Status().value("retiring").toBool(), "underlying work is still draining");
            Check(!client.Submit(image).value("ok").toBool(), "no overlapping work while draining");
            Wait([&] { return !client.Status().value("running").toBool(); });
            Check(client.Result(id).value("state") == "failed" && completions == 1, "discard late completion exactly once");
            qunsetenv("DESKWAIFU_OCR_TEST_MODE");
            // 该用例的超时期限（cancel ? 1000ms : 50ms）本身有含义：50ms 期限是故意用来
            // 触发“请求超时”的，因此超时后引擎会被释放，恢复性请求需要重新冷加载模块。
            // 实测冷加载耗时 1~90ms 且随机器负载波动，CI 上偶发超过 50ms，于是同一实例上的
            // 恢复性识别会再次被 50ms 期限打断，把“模块加载慢”误判成“超时后不可恢复”，
            // 造成与产品逻辑无关的偶发失败（event-loop wait timed out）。
            // 这里的正确契约是“超时是暂时性错误、可重试”，而不是“50ms 内必须重新识别成功”，
            // 所以按契约断言，不依赖具体耗时；取消用例期限充裕，仍在同一实例上验证可继续识别。
            if (cancel) {
                Recognize(client, image);
            } else {
                // 超时是可重试的暂时性错误，不能被当成永久失效（response 里只带 error 文本，
                // 分类码由 Status().error_code 暴露，Submit 正是依据它判断可恢复）。
                Check(client.Status().value("error_code").toString() == "timeout", "deadline reports a retryable timeout");
                Check(client.Submit(image).value("ok").toBool(), "timeout leaves client recoverable");
                // 恢复性请求同样会在 50ms 后超时，这里只等待它彻底结束，不要求识别成功。
                Wait([&] { return !client.Status().value("running").toBool(); });
            }
        }
    }
    for (const QJsonValue value : { QJsonValue("relative"), QJsonValue(""), QJsonValue(42), QJsonValue(QJsonValue::Null) }) {
        OcrClient client(module);
        client.ConfigureModels({ { "ocr_detection_model_dir", value } });
        Check(client.Status().value("error_code") == "invalid_config", "invalid configured directory");
        Check(!client.Status().value("running").toBool(), "invalid config does not load engine");
        Check(!client.Submit(image).value("ok").toBool(), "invalid config rejects task");
    }
    {
        OcrClient client(module);
        client.ConfigureModels({ }, "Malformed configuration JSON");
        Check(client.Status().value("error_code") == "invalid_config", "configuration read error disables OCR");
    }
    {
        qputenv("DESKWAIFU_OCR_TEST_MODE", "arguments");
        OcrClient client(module, 1000, 30);
        client.ConfigureModels({ { "ocr_detection_model_dir", "/tmp/检测 模型" }, { "ocr_recognition_model_dir", "/tmp/识别 模型" } });
        Check(client.Status().value("availability_state") == "checking", "asynchronous startup validation");
        Check(!client.Submit(image).value("ok").toBool(), "checking rejects tasks");
        Wait([&] { return client.Status().value("available").toBool(); });
        Check(client.Status().value("state") == "idle", "validation is not a recognition task");
        Recognize(client, image);
        Wait([&] { return !client.Status().value("running").toBool(); });
        Check(client.Status().value("available").toBool(), "idle release retains last validated availability");
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    }
    {
        qputenv("DESKWAIFU_OCR_TEST_MODE", "load-error");
        OcrClient client(module, 1000, 30);
        client.ConfigureModels({ });
        Wait([&] { return client.Status().value("availability_state") == "unavailable"; });
        Wait([&] { return !client.Status().value("running").toBool(); });
        Check(!client.Submit(image).value("ok").toBool(), "model load failure stays disabled without retry loop");
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    }
    // Destruction during initialization/inference joins safely, never unloading executing code.
    for (const auto* mode : { "slow-load", "hang", "slow-release" }) {
        qputenv("DESKWAIFU_OCR_TEST_MODE", mode);
        auto client = std::make_unique<OcrClient>(module, 1000, 30);
        client->Submit(image);
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 20) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        client.reset();
        QCoreApplication::processEvents();
        qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    }
    for (int i = 0; i < 10; ++i) {
        OcrClient client(module, 1000, 5);
        Recognize(client, image);
        Wait([&] { return !client.Status().value("running").toBool(); });
    }
    {
        OcrClient first(module, 1000, 5), second(module, 1000, 1000);
        const auto a = Recognize(first, image);
        const auto b = Recognize(second, image);
        Check(a.value("instance") != b.value("instance"), "each client owns an independent model session");
        Wait([&] { return !first.Status().value("running").toBool(); });
        Check(Recognize(second, image).value("instance") == b.value("instance"), "another client's release cannot destroy active session");
    }
    std::cout << "OCR in-process lifecycle, responsiveness and compatibility tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
