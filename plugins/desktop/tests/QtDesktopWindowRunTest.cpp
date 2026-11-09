// Include the private production class to exercise its real Run/StartFrames
// implementation. Only the availability/GL boundary is replaced; no compositor
// or user desktop is involved.
#include "../private/processor/interaction/window/QtDesktopWindow.cpp"
#include <QElapsedTimer>
#include <QThread>
#include <cstdlib>

namespace {
void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::abort(); }
}
class Window final : public QtDesktopWindow
{
public:
    bool Available() const override { return available; }
    bool MakeCurrent() override { ++attempts; return false; }
    bool available = true;
    int attempts = 0;
};
void DispatchLaterEvents()
{
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 50) { QCoreApplication::processEvents(); QThread::msleep(1); }
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    Window window;
    for (int i = 0; i < 3; ++i) {
        QTimer::singleShot(0, [&] {
            QTimer::singleShot(0, [] { QCoreApplication::exit(17); });
            Check(window.Run([] { std::abort(); return true; }), "external exit preserves Run result");
        });
        Check(app.exec() == 17, "external exit terminates the nested and application loops");
        DispatchLaterEvents();
        Check(window.attempts == 0, "Run must retire the frame timer before stack captures die");
    }
    QTimer::singleShot(0, [&] {
        Check(!window.Run([] { return true; }), "frame failure reports false");
        app.quit();
    });
    app.exec();
    Check(window.attempts == 1, "normal completion executes one frame timer callback");
    DispatchLaterEvents();
    Check(window.attempts == 1, "normal Run completion also retires its timer");
    window.available = false;
    Check(!window.Run([] { return true; }), "unavailable window rejects Run");
    DispatchLaterEvents();
    Check(window.attempts == 1, "rejected Run leaves no callbacks");
    std::cout << "Real Qt window Run: external exit, late dispatch and repeated lifecycle passed\n";
}
