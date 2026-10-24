#include "event/ProcessorEventLoop.hpp"
#include <QCoreApplication>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Drain() { for (int i = 0; i < 30; ++i) QCoreApplication::processEvents(); }
struct Handler : IProcessor
{
    std::function<bool(const ProcessorEvent&)> callback;
    bool HandleEvent(const ProcessorEvent& event) override { return callback(event); }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    Handler a, b, c;
    QStringList calls;
    a.callback = [&](const ProcessorEvent& e) { calls.append("a:" + e.type); return false; };
    b.callback = [&](const ProcessorEvent& e) { calls.append("b:" + e.type); return true; };
    c.callback = [&](const ProcessorEvent& e) { calls.append("c:" + e.type); return false; };
    Check(loop.Register(&a) && loop.Register(&b) && loop.Register(&c), "register handlers");
    Check(!loop.Register(&a) && !loop.Register(nullptr), "reject duplicate or null registration");
    Check(loop.Post({"broadcast"}), "queue broadcast");
    Check(calls.empty(), "posting must be asynchronous");
    Drain();
    Check(calls == QStringList{"a:broadcast", "b:broadcast"}, "true stops ordered dispatch");
    calls.clear();
    loop.Post({"direct", &a, &c}); Drain();
    Check(calls == QStringList{"c:direct"}, "targeted false must not fall through to other handlers");
    calls.clear();
    loop.Post({"drop-receiver", &a, &c});
    loop.Post({"drop-sender", &c, &a});
    loop.Unregister(&c);
    loop.Unregister(nullptr);
    loop.Post({"keep", nullptr, &a}); Drain();
    Check(calls == QStringList{"a:keep"}, "unregistration purges queued pointers without purging unrelated events");
    Check(!loop.Post({"stale", &c, &a}) && !loop.Post({"stale", &a, &c}), "reject unregistered pointers");

    calls.clear();
    a.callback = [&](const ProcessorEvent& e) {
        calls.append("begin:" + e.type);
        if (e.type == "outer") {
            loop.Post({"inner", &a, &a});
            QCoreApplication::processEvents();
            calls.append("end:outer");
        }
        return true;
    };
    loop.Post({"outer", nullptr, &a}); Drain();
    Check(calls == QStringList{"begin:outer", "end:outer", "begin:inner"}, "non-recursive FIFO dispatch");

    calls.clear();
    a.callback = [&](const ProcessorEvent& e) {
        Check(QThread::currentThread() == app.thread(), "callback belongs to main thread");
        calls.append(e.type); return true;
    };
    bool posted = false;
    std::thread worker([&] { posted = loop.Post({"worker-result", nullptr, &a}); });
    worker.join(); Drain();
    Check(posted && calls == QStringList{"worker-result"}, "worker results return through main event loop");

    calls.clear();
    loop.Register(&c);
    a.callback = [&](const ProcessorEvent&) { calls.append("a"); loop.Unregister(&b); return false; };
    c.callback = [&](const ProcessorEvent&) { calls.append("c"); return false; };
    loop.Post({"remove-during-dispatch"}); Drain();
    Check(calls == QStringList{"a", "c"}, "removed handler must not be called from dispatch snapshot");
    calls.clear();
    loop.Register(&b);
    a.callback = [&](const ProcessorEvent&) { calls.append("a"); loop.Unregister(&b); loop.Register(&b); return false; };
    loop.Post({"reregister-during-dispatch"}); Drain();
    Check(calls == QStringList{"a", "c"}, "new registration at the same pointer is not part of current dispatch");
    loop.Unregister(&b);
    calls.clear();
    a.callback = [&](const ProcessorEvent&) { calls.append("a"); loop.Unregister(&a); return false; };
    loop.Post({"sender-removed", &a}); Drain();
    Check(calls == QStringList{"a"}, "stop forwarding an event whose sender has unregistered");
    loop.Unregister(&c);
    std::cout << "Pointer-based event registration, consumption and lifecycle tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
