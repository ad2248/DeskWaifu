#include "SignalProcessor.hpp"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <thread>

namespace {
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Signal handlers require lock-free atomics");
std::atomic<int> signalPipe{-1}, activeHandlers{0};
void ReceiveSignal(int signal)
{
    const int previous = errno;
    ++activeHandlers;
    const unsigned char action = signal == SIGUSR1 ? 1 : 2;
    const int descriptor = signalPipe.load();
    if (descriptor >= 0) {
        const auto ignored = write(descriptor, &action, 1);
        (void)ignored; // A full pipe coalesces excess activation, never blocks a signal handler.
    }
    --activeHandlers;
    errno = previous;
}
}

SignalProcessor::SignalProcessor(ProcessorEventLoop& loop, std::function<void()> toggle, std::function<void()> focus)
    : Processor(loop), _toggle(std::move(toggle)), _focus(std::move(focus))
{
    int descriptors[2];
    if (signalPipe >= 0 || pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) != 0) return;
    _read = descriptors[0]; _write = descriptors[1];
    struct sigaction action{};
    action.sa_handler = ReceiveSignal;
    sigemptyset(&action.sa_mask);
    signalPipe = _write;
    if (sigaction(SIGUSR1, &action, &_oldToggle) != 0) {
        signalPipe = -1; return;
    }
    if (sigaction(SIGUSR2, &action, &_oldFocus) != 0) {
        sigaction(SIGUSR1, &_oldToggle, nullptr); signalPipe = -1; return;
    }
    _notifier.reset(new QSocketNotifier(_read, QSocketNotifier::Read));
    connect(_notifier.get(), &QSocketNotifier::activated, this, [this] {
        unsigned char actions[64];
        // Bound each notification. Remaining bytes trigger readiness again.
        const auto count = read(_read, actions, sizeof(actions));
        for (ssize_t i = 0; i < count; ++i)
            Send(actions[i] == 1 ? QStringLiteral("signal.toggle") : QStringLiteral("signal.focus"), this);
    });
}

SignalProcessor::~SignalProcessor()
{
    if (_notifier) {
        _notifier.reset();
        signalPipe = -1;
        sigaction(SIGUSR1, &_oldToggle, nullptr); sigaction(SIGUSR2, &_oldFocus, nullptr);
    }
    // A handler already running on another thread may still hold the old fd.
    // Stop it before closing/reusing either pipe endpoint (and avoid SIGPIPE).
    if (_write >= 0) while (activeHandlers.load() != 0) std::this_thread::yield();
    if (_read >= 0) close(_read);
    if (_write >= 0) close(_write);
}

bool SignalProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type == QStringLiteral("signal.toggle")) { _toggle(); return true; }
    if (event.type == QStringLiteral("signal.focus")) { _focus(); return true; }
    return false;
}
