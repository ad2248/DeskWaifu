#include "processor/signal/SignalProcessor.hpp"
#include <QCoreApplication>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)
namespace { volatile sig_atomic_t restored = 0; void Previous(int) { restored = 1; } }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    struct sigaction previous{}, old{};
    previous.sa_handler = Previous; sigemptyset(&previous.sa_mask);
    CHECK(sigaction(SIGUSR1, &previous, &old) == 0);
    int toggles = 0, focuses = 0;
    {
        SignalProcessor processor(loop, [&] { ++toggles; }, [&] { ++focuses; });
        CHECK(processor.Available());
        CHECK(raise(SIGUSR1) == 0 && raise(SIGUSR2) == 0);
        CHECK(toggles == 0 && focuses == 0);
        for (int i = 0; i < 8; ++i) QCoreApplication::processEvents();
        CHECK(toggles == 1 && focuses == 1 && !restored);
        CHECK(!processor.HandleEvent({"unrelated"}));
        CHECK(raise(SIGUSR1) == 0); // Destruction purges undelivered work.
    }
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    CHECK(toggles == 1 && focuses == 1);
    CHECK(raise(SIGUSR1) == 0 && restored);
    CHECK(sigaction(SIGUSR1, &old, nullptr) == 0);
    return 0;
}
