#include "processor/ipc/IpcProcessor.hpp"
#include "EventLoopFence.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "IPC readiness timed out");
}
struct Commands : ICommandService
{
    int count = 0, disconnected = 0;
    bool large = false;
    std::string Execute(const std::string&, const std::string&) override
    { ++count; return large ? std::string(128 * 1024, 'x') : "{\"ok\":true}"; }
    void Disconnected(const std::string&) override { ++disconnected; }
};
}
int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    QTemporaryDir runtime; Check(runtime.isValid(), "private IPC runtime"); qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    Commands commands; LAppIPC ipc; ipc.SetCommandService(commands); Check(ipc.Initialize(), "initialize private Unix socket");
    ProcessorEventLoop loop; IpcProcessor ipcEvents(loop, ipc);
    QLocalSocket socket; socket.connectToServer(QString::fromStdString(ipc.GetSocketPath()));
    Check(socket.waitForConnected(1000), "connect private IPC client");
    ipcEvents.SetPaused(true);
    QByteArray requests; for (int i = 0; i < 40; ++i) requests += "{}\n";
    socket.write(requests); socket.flush(); Check(AwaitProcessorEvents(loop), "processor FIFO fence timed out");
    Check(commands.count == 0, "paused IPC must not run inside output replacement");
    ipcEvents.SetPaused(false);
    Wait([&] { return commands.count == 40; });
    QByteArray replies;
    Wait([&] { replies += socket.readAll(); return replies.count('\n') == 40; });
    Check(commands.count == 40, "buffered commands drain beyond per-poll budget without any render frames");
    commands.large = true;
    socket.write("{}\n{}\n"); socket.flush(); replies.clear();
    Wait([&] { replies += socket.readAll(); return replies.count('\n') == 2; });
    Check(replies.size() == 2 * (128 * 1024 + 1), "write readiness flushes replies beyond transport write budget");
    socket.disconnectFromServer();
    Wait([&] { return commands.disconnected == 1; });
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
