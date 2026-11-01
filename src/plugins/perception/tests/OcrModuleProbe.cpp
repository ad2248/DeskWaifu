// Test-only host for exercising the real in-process module without a compositor.
#include "processor/ocr/OcrClient.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QThread>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>

namespace {
void Emit(QJsonObject object)
{
    object.insert("protocol", 1);
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
}

qint64 MemoryKiB(const QByteArray& field)
{
    QFile file("/proc/self/status");
    if (!file.open(QIODevice::ReadOnly)) return -1;
    for (const auto& line : file.readAll().split('\n'))
        if (line.startsWith(field + ':')) return line.simplified().split(' ').value(1).toLongLong();
    return -1;
}

template<class Predicate> void Wait(Predicate predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate()) {
        if (timer.elapsed() > 65000) throw std::runtime_error("module probe wait timed out");
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto arguments = app.arguments(); arguments.removeFirst();
    QString module;
    QJsonObject config;
    bool serve = false;
    try {
        while (!arguments.isEmpty()) {
            const auto option = arguments.takeFirst();
            if (option == "--serve" && !serve) { serve = true; continue; }
            const auto key = option == "--detection-model-dir" ? "ocr_detection_model_dir" : "ocr_recognition_model_dir";
            if ((option != "--module" && option != "--detection-model-dir" && option != "--recognition-model-dir") ||
                arguments.isEmpty() || arguments.first().startsWith("--") ||
                (option == "--module" ? !module.isEmpty() : config.contains(key)))
                throw std::runtime_error("Unknown, duplicate or missing test-probe argument");
            if (option == "--module") module = arguments.takeFirst();
            else config.insert(key, arguments.takeFirst());
        }
        if (!serve || module.isEmpty()) throw std::runtime_error("Expected --module and --serve");
    } catch (const std::exception& error) {
        Emit({{"event", "error"}, {"code", "invalid_arguments"}, {"error", error.what()}}); return 2;
    }
    try {
        OcrClient client(module, 60000, 20);
        QElapsedTimer initialization; initialization.start();
        client.ConfigureModels(config);
        Wait([&] { return client.Status().value("availability_state") != "checking"; });
        const auto status = client.Status();
        if (!status.value("available").toBool()) {
            Emit({{"event", "error"}, {"code", status.value("error_code")}, {"error", status.value("error")}}); return 1;
        }
        Emit({{"event", "ready"}, {"backend", "onnxruntime-cpu"}, {"execution_mode", "in_process"},
              {"initialization_ms", initialization.elapsed()},
              {"detection_model", "PP-OCRv6_small_det"}, {"recognition_model", "PP-OCRv6_small_rec"}});
        std::string line;
        while (std::getline(std::cin, line)) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
            const auto request = document.object();
            const auto id = request.value("id");
            if (request.value("probe_action") == QJsonValue("idle")) {
                Wait([&] { return !client.Status().value("running").toBool(); });
                Emit({{"event", "idle"}, {"running", false}, {"rss_kib", MemoryKiB("VmRSS")}});
                continue;
            }
            if (line.size() > 64 * 1024 || error.error != QJsonParseError::NoError || !document.isObject() ||
                request.value("protocol") != QJsonValue(1) || !id.isString() || id.toString().isEmpty() ||
                !request.value("image").isString()) {
                Emit({{"id", id}, {"ok", false}, {"error", "invalid test-probe request"}}); continue;
            }
            auto accepted = client.Submit(request.value("image").toString());
            if (!accepted.value("ok").toBool()) { accepted.insert("id", id); Emit(accepted); continue; }
            const auto job = accepted.value("job_id").toString();
            Wait([&] { return client.Result(job).value("state") != "pending"; });
            auto reply = client.Result(job).value("response").toObject();
            reply.insert("id", id); Emit(reply);
        }
        const auto loadedRss = MemoryKiB("VmRSS");
        Wait([&] { return !client.Status().value("running").toBool(); });
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cerr << QJsonDocument(QJsonObject{{"probe_metrics", true}, {"loaded_rss_kib", loadedRss},
            {"idle_rss_kib", MemoryKiB("VmRSS")}, {"peak_rss_kib", qint64(usage.ru_maxrss)}}).toJson(QJsonDocument::Compact).constData() << '\n';
    } catch (const std::exception& error) {
        Emit({{"event", "error"}, {"code", "probe_failed"}, {"error", error.what()}}); return 1;
    }
    return 0;
}
