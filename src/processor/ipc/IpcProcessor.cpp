#include "IpcProcessor.hpp"
#include "processor/Events.hpp"
#include <QPointer>

IpcProcessor::IpcProcessor(ProcessorEventLoop& loop, LAppIPC& ipc) : Processor(loop), _ipc(ipc)
{
    QPointer<IpcProcessor> self(this);
    _ipc.SetReadyCallback([self] { if (self) self->Schedule(); });
    Refresh();
}

IpcProcessor::IpcProcessor(ProcessorEventLoop& loop, std::unique_ptr<LAppIPC> ipc, IProcessor& commands)
    : Processor(loop), _owned(std::move(ipc)), _ipc(*_owned), _commands(&commands)
{
    _ipc.SetCommandService(*this);
}

IpcProcessor::~IpcProcessor()
{
    Stop();
    // transport 析构仍会通知 Disconnected；此时请求表和 Processor 基类必须存活。
    _owned.reset();
}
bool IpcProcessor::Start()
{
    if (!_ipc.Initialize()) return false;
    QPointer<IpcProcessor> self(this);
    _ipc.SetReadyCallback([self] { if (self) self->Schedule(); });
    Refresh(); return true;
}
void IpcProcessor::Stop()
{
    _stopped = true; _notifiers.clear(); _ipc.SetReadyCallback({});
}
void IpcProcessor::ExecuteAsync(const std::string& json, const std::string& connection, std::function<void(std::string)> complete)
{
    if (_stopped) { complete(R"({"ok":false,"error":"IPC stopped"})"); return; }
    auto inserted = _requests.emplace(connection, Request{json, connection, std::move(complete)});
    Dispatch(inserted.first->second);
}
void IpcProcessor::Dispatch(const Request& request)
{
    Send("command.request", _commands, {{"json", QString::fromStdString(request.json)},
        {"connection", QString::fromStdString(request.connection)}});
}
void IpcProcessor::Disconnected(const std::string& connection)
{
    _requests.erase(connection);
    Send("command.disconnected", nullptr, {{"connection", QString::fromStdString(connection)}});
}

void IpcProcessor::Schedule()
{
    if (_queued || _paused || _stopped) return;
    _queued = true;
    for (const auto& notifier : _notifiers) notifier->setEnabled(false);
    Send(ProcessorEvents::IpcReady, this);
}

void IpcProcessor::SetPaused(bool paused)
{
    _paused = paused;
    if (paused) for (const auto& notifier : _notifiers) notifier->setEnabled(false);
    else Schedule();
}

void IpcProcessor::Refresh()
{
    _notifiers.clear();
    auto add = [this](int fd, QSocketNotifier::Type type) {
        auto notifier = std::make_unique<QSocketNotifier>(fd, type);
        connect(notifier.get(), &QSocketNotifier::activated, this, [this] { Schedule(); });
        _notifiers.push_back(std::move(notifier));
    };
    for (int fd : _ipc.ReadDescriptors()) add(fd, QSocketNotifier::Read);
    for (int fd : _ipc.WriteDescriptors()) add(fd, QSocketNotifier::Write);
    if (_ipc.HasBufferedWork()) Schedule();
}

bool IpcProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type == "window.switch") { SetPaused(event.data.value("paused").toBool()); return false; }
    if (event.type == "ipc.command.result" && event.sender == _commands) {
        const auto found = _requests.find(event.data.value("connection").toString().toStdString());
        if (found != _requests.end()) {
            auto request = std::move(found->second); _requests.erase(found);
            request.complete(event.data.value("json").toString().toStdString());
        }
        if (event.data.value("quit").toBool()) Send("application.quit");
        return true;
    }
    if (event.type != ProcessorEvents::IpcReady) return false;
    _queued = false;
    if (_paused || _stopped) return true;
    _ipc.Poll();
    Refresh();
    return true;
}
