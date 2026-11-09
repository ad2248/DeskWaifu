#include "ClipboardService.hpp"
#include "contracts/ClipboardEvents.hpp"
#include "contracts/ConfigStore.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <signal.h>
#include <unistd.h>

namespace {
void KillPendingProcess(QProcess* process)
{
    // The child modifier gives this invocation a private session/process group.
    // A Starting process may not yet have a PID/group; the started hook retries.
    const auto pid = process->processId();
    if (pid > 0) ::kill(-pid_t(pid), SIGKILL);
    process->kill();
}
}

ClipboardService::ClipboardService(EventEndpoint& endpoint, QString program)
    : _endpoint(endpoint), _program(std::move(program))
{
    _deadline.setSingleShot(true);
    _deadline.setInterval(ClipboardEvents::DeadlineMs);
    connect(&_deadline, &QTimer::timeout, this, [this] {
        Complete(false, "timeout", "Clipboard write deadline exceeded", "unknown");
        if (_process) KillPendingProcess(_process);
    });
}

ClipboardService::~ClipboardService() { Stop(); }

void ClipboardService::Reply(RequestId id, bool external, const QString& token, bool hasToken,
                             bool ok, const QString& code, const QString& error, const QString& state)
{
    QVariantMap result{{"ok", ok}, {"error_code", code}, {"error", error}, {"copy_state", state}};
    if (hasToken) result.insert("requestToken", token);
    _endpoint.Respond(id, std::make_shared<ProcessorEvent>(external ? "command.result" : ClipboardEvents::WriteResult,
        nullptr, nullptr, external ? QVariantMap{{"json", QString::fromUtf8(QJsonDocument(
            QJsonObject::fromVariantMap(result)).toJson(QJsonDocument::Compact))}} : result));
}

void ClipboardService::Complete(bool ok, const QString& code, const QString& error, const QString& state)
{
    _deadline.stop();
    const auto id = _request;
    _request = 0;
    if (id) Reply(id, _external, _token, _hasToken, ok, code, error, state);
    _token.clear();
}

void ClipboardService::Deliver(const EventDelivery& delivery)
{
    if (_stopped) return;
    if (delivery.kind == EventDelivery::Kind::Cancellation) {
        if (delivery.requestId == _request) {
            _request = 0; _token.clear(); _deadline.stop();
            if (_process) KillPendingProcess(_process);
        }
        return;
    }
    if (delivery.kind != EventDelivery::Kind::Request) return;
    const bool external = delivery.topic == ClipboardEvents::CommandCapability;
    auto reject = [&](const QString& code, const QString& message) {
        Reply(delivery.requestId, external, {}, false, false, code, message, "failed");
    };
    if (!delivery.payload || (delivery.topic != ClipboardEvents::WriteTextCapability && !external)) {
        reject("invalid_request", "Invalid clipboard capability"); return;
    }
    const auto& payload = *delivery.payload;
    QVariantMap data = payload.data;
    if (external) {
        const bool hasConnection = data.contains("connection");
        if (payload.type != "command.invoke" || data.size() != (hasConnection ? 2 : 1) ||
            data.value("json").metaType().id() != QMetaType::QString ||
            (hasConnection && data.value("connection").metaType().id() != QMetaType::QString)) {
            reject("invalid_request", "Expected command.invoke with JSON string"); return;
        }
        const auto json = data.value("json").toString().toUtf8();
        // Escaped JSON can use six bytes per text character, plus bounded envelope space.
        if (json.size() > ClipboardEvents::MaxTextBytes * 6 + 4096) {
            reject("too_large", "Clipboard request exceeds size limit"); return;
        }
        ConfigStore::Snapshot snapshot; std::string error;
        if (!ConfigStore::Parse(json.toStdString(), snapshot, error, false)) {
            reject("invalid_request", "Invalid JSON or duplicate key"); return;
        }
        const auto document = QJsonDocument::fromJson(json);
        if (!document.isObject()) { reject("invalid_request", "Expected JSON object"); return; }
        const auto object = document.object();
        if (!object.value("command").isString() || object.value("command").toString() != "write_clipboard") {
            reject("invalid_request", "Command capability mismatch"); return;
        }
        data = object.toVariantMap(); data.remove("command");
    } else if (payload.type != ClipboardEvents::WriteText) {
        reject("invalid_request", "Expected clipboard.write_text"); return;
    }
    const bool hasToken = data.contains("requestToken");
    if (data.value("text").metaType().id() != QMetaType::QString ||
        (hasToken && data.value("requestToken").metaType().id() != QMetaType::QString) ||
        data.size() != (hasToken ? 2 : 1)) {
        reject("invalid_request", "Expected text and optional requestToken strings only"); return;
    }
    const auto text = data.value("text").toString().toUtf8();
    if (text.size() > ClipboardEvents::MaxTextBytes ||
        (hasToken && data.value("requestToken").toString().toUtf8().size() > ClipboardEvents::MaxTextBytes)) {
        reject("too_large", "Clipboard text or token exceeds 1 MiB"); return;
    }
    if (_process) { reject("busy", "Clipboard write already in progress"); return; }
    _request = delivery.requestId; _external = external; _hasToken = hasToken;
    _token = data.value("requestToken").toString();
    _process = new QProcess(this);
    _process->setChildProcessModifier([] { if (::setsid() < 0) ::_exit(127); });
    // Discard backend output: no user text or compositor diagnostics enter replies/logs.
    _process->setStandardOutputFile(QProcess::nullDevice());
    _process->setStandardErrorFile(QProcess::nullDevice());
    connect(_process, &QProcess::started, this, [this, text] {
        if (!_request) { KillPendingProcess(_process); return; }
        if (_process->write(text) != text.size()) {
            Complete(false, "write_failed", "Unable to send clipboard text", "unknown");
            KillPendingProcess(_process); return;
        }
        _process->closeWriteChannel();
    });
    connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            Complete(false, "unavailable", "Unable to start clipboard backend", "failed");
            auto* process = _process; _process = nullptr; process->deleteLater();
        } else if (error == QProcess::WriteError || error == QProcess::ReadError) {
            Complete(false, "write_failed", "Clipboard backend I/O failed", "unknown");
            KillPendingProcess(_process);
        }
    });
    connect(_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus status) {
            const bool ok = status == QProcess::NormalExit && code == 0;
            Complete(ok, ok ? "" : "backend_failed", ok ? "" : "Clipboard backend failed",
                     ok ? "committed" : "unknown");
            auto* process = _process; _process = nullptr; process->deleteLater();
        });
    _deadline.start();
    // Do not use --foreground: wl-copy's successful child remains the selection holder.
    _process->start(_program, {"--type", "text/plain;charset=utf-8"});
}

void ClipboardService::Stop() noexcept
{
    if (_stopped) return;
    _stopped = true; _deadline.stop(); _request = 0; _token.clear();
    if (!_process) return;
    auto* process = _process; _process = nullptr;
    process->disconnect(this);
    // QProcess destruction waits for a live child: reap asynchronously instead.
    process->setParent(QCoreApplication::instance());
    // kill() during Starting may precede the child PID becoming available.
    connect(process, &QProcess::started, process, [process] { KillPendingProcess(process); });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) process->deleteLater();
    });
    if (process->state() == QProcess::NotRunning) process->deleteLater();
    else KillPendingProcess(process);
}
