#include "processor/interaction/window/DesktopWindow.hpp"
#include "fake-input-client.h"
#include <QGuiApplication>
#include <QBackingStore>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QWindow>
#include <LayerShellQt/Window>
#include <wayland-client.h>
#include <linux/input-event-codes.h>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace {
void Require(bool ok, const char* message) { if (!ok) { std::cerr << message << '\n'; std::abort(); } }
struct Events : IDesktopWindowEvents
{
    int presses = 0;
    bool inside = false;
    void PointerEntered(double, double) override { inside = true; }
    void PointerLeft() override { inside = false; }
    void PointerMoved(double, double) override {}
    void PointerButton(int button, bool down) override { if (button == 0 && down) ++presses; }
    void PointerScroll(double) override {}
    void KeyboardChanged() override {}
    void KeyboardKey(uint32_t, bool) override {}
    void SurfaceClosed() override {}
    void OutputSwitchStarted() override {}
    void OutputSwitchFinished() override {}
};
class Below : public QWindow
{
public:
    QBackingStore backing{this};
    int presses = 0;
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::MouseButtonPress) ++presses;
        if (event->type() == QEvent::Expose && isExposed()) {
            backing.resize(size()); backing.beginPaint(QRect(QPoint{}, size()));
            QPainter paint(backing.paintDevice()); paint.fillRect(QRect(QPoint{}, size()), Qt::green); paint.end();
            backing.endPaint(); backing.flush(QRect(QPoint{}, size()));
        }
        return QWindow::event(event);
    }
};
struct Input
{
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    org_kde_kwin_fake_input* device = nullptr;
    static void Global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
    {
        if (std::strcmp(interface, "org_kde_kwin_fake_input") == 0 && version >= 3)
            static_cast<Input*>(data)->device = static_cast<org_kde_kwin_fake_input*>(
                wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, 3));
    }
    static void Removed(void*, wl_registry*, uint32_t) {}
    Input()
    {
        display = wl_display_connect(nullptr); Require(display, "connect to private compositor");
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{Global, Removed};
        wl_registry_add_listener(registry, &listener, this);
        Require(wl_display_roundtrip(display) >= 0 && device, "KWin fake-input v3 is required for actual click-through evidence");
        org_kde_kwin_fake_input_authenticate(device, "DeskWaifu regression", "private headless click-through test");
        Require(wl_display_roundtrip(display) >= 0, "authenticate fake input");
    }
    void Move(int x, int y)
    {
        org_kde_kwin_fake_input_pointer_motion_absolute(device, wl_fixed_from_int(x), wl_fixed_from_int(y));
        Require(wl_display_roundtrip(display) >= 0, "compositor motion completion barrier");
    }
    void Click(int x, int y)
    {
        Move(x, y);
        org_kde_kwin_fake_input_button(device, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
        org_kde_kwin_fake_input_button(device, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
        Require(wl_display_roundtrip(display) >= 0, "compositor input completion barrier");
    }
    ~Input() { org_kde_kwin_fake_input_destroy(device); wl_registry_destroy(registry); wl_display_disconnect(display); }
};
}

int main(int argc, char** argv)
{
    // Refuse to inject into an inherited real desktop by accident.
    Require(qEnvironmentVariable("DESKWAIFU_TEST_ISOLATED") == "1" && qEnvironmentVariable("WAYLAND_DISPLAY") == "qt-model-test",
        "click injection requires the private KWin smoke launcher");
    QGuiApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    Below below;
    auto* background = LayerShellQt::Window::get(&below);
    background->setLayer(LayerShellQt::Window::LayerBackground);
    background->setScreen(app.screens().front());
    background->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorBottom
        | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
    background->setExclusiveZone(-1);
    below.show();
    QElapsedTimer exposed; exposed.start();
    while (!below.isExposed() && exposed.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Require(below.isExposed(), "underlying test window must be exposed");
    Events events;
    auto window = CreateDesktopWindow();
    Require(window->Initialize(640, 480, events), "initialize production overlay");
    Input input;
    const auto outputs = window->Outputs();
    const auto selected = std::find_if(outputs.begin(), outputs.end(), [&](const OutputSnapshot& value) { return value.id == window->CurrentOutput(); });
    Require(selected != outputs.end(), "selected overlay output missing");
    const auto output = *selected;
    auto click = [&](int x, int y) { input.Click(output.x + x, output.y + y); };
    const int x = window->Width() / 4, y = window->Height() / 4;
    const std::vector<ModelRegion> shaped{{x - 10, y - 10, 40, 40}};
    int stage = 0;
    QElapsedTimer deadline; deadline.start();
    Require(window->Run([&] {
        if (deadline.elapsed() >= 10000) {
            std::cerr << "click-through timeout: stage=" << stage << "; overlay=" << events.presses << "; below=" << below.presses << '\n';
            return false;
        }
        auto* gl = QOpenGLContext::currentContext()->extraFunctions();
        gl->glClearColor(0, 0, 0, 0); gl->glClear(GL_COLOR_BUFFER_BIT);
        switch (stage) {
        case 0: Require(window->SetInputRegion(shaped), "set shaped input region"); ++stage; break;
        case 1:
            // Compositor effects and mapping are asynchronous. Observe actual
            // pointer focus before sending the first irreversible button event.
            if (events.inside) { click(x, y); ++stage; }
            else input.Move(output.x + x, output.y + y);
            break;
        case 2:
            if (events.presses == 1) { Require(below.presses == 0, "opaque input region leaked a click"); window->SetInputRegion({}); ++stage; }
            break;
        case 3: click(x, y); ++stage; break;
        case 4:
            if (below.presses == 1) { Require(events.presses == 1, "empty input region retained a click"); window->SetInputRegion(shaped); ++stage; }
            break;
        case 5: click(window->Width() * 3 / 4, window->Height() * 3 / 4); ++stage; break;
        case 6:
            if (below.presses == 2) { Require(events.presses == 1, "outside shaped region retained a click"); ++stage; }
            break;
        case 7: click(x, y); ++stage; break;
        case 8:
            if (events.presses == 2) { Require(below.presses == 2, "restored region leaked a click"); return false; }
            break;
        }
        return true;
    }), "production window loop failed");
    Require(stage == 8 && events.presses == 2 && below.presses == 2, "click-through scenario did not reach its terminal barrier");
    window->Shutdown();
    std::cout << "Actual compositor click routing passed: overlay=" << events.presses << "; below=" << below.presses << '\n';
}
