#include "DesktopWindow.hpp"
#include <QGuiApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QProcess>
#include <QWindow>
#include <QEventLoop>
#include <QTimer>
#include <EGL/egl.h>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::abort(); }
}
QWindow* pixelWindow = nullptr;
bool checkWindowPixels = false;
int presentedFrames = 0;
struct Events : IDesktopWindowEvents
{
    int presses = 0, releases = 0, resets = 0;
    int switchesStarted = 0, switchesFinished = 0, closes = 0;
    uint32_t key = 0;
    double x = 0, y = 0, scroll = 0;
    void PointerEntered(double px, double py) override { x = px; y = py; }
    void PointerLeft() override {}
    void PointerMoved(double px, double py) override { x = px; y = py; }
    void PointerButton(int button, bool down) override { if (!button) down ? ++presses : ++releases; }
    void PointerScroll(double delta) override { scroll += delta; }
    void KeyboardChanged() override { ++resets; }
    void KeyboardKey(uint32_t code, bool) override { key = code; }
    void SurfaceClosed() override { ++closes; }
    void OutputSwitchStarted() override
    {
        ++switchesStarted;
        const int before = presses;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(25, 35), QPointF(25, 35),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(pixelWindow, &press);
        Require(presses == before, "Input must be blocked during output replacement");
        const int previousCloses = closes;
        QCloseEvent close;
        QCoreApplication::sendEvent(pixelWindow, &close);
        Require(closes == previousCloses + 1 && !close.isAccepted(),
            "Close during output replacement must notify policy without destroying the surface");
    }
    void OutputSwitchFinished() override { ++switchesFinished; }
};

// A pointer-triggered output rebind runs outside the frame callback. Delay an
// expose event so the frame timer definitely becomes ready in its nested loop.
struct SlowExpose : QObject
{
    bool switching = false;
    bool delayed = false;
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (switching && event->type() == QEvent::Expose) {
            delayed = true;
            QEventLoop wait;
            QTimer::singleShot(80, &wait, &QEventLoop::quit);
            wait.exec();
        }
        return false;
    }
};
}

// Inspect the actual window back buffer immediately before presentation, not
// the model FBO (which can contain pixels even when nothing reaches the screen).
extern "C" EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    if (checkWindowPixels) {
        auto* context = QOpenGLContext::currentContext();
        auto* gl = context->extraFunctions();
        GLint previous;
        gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, context->defaultFramebufferObject());
        gl->glReadBuffer(GL_BACK);
        unsigned char pixel[4] = {};
        const qreal dpr = pixelWindow->devicePixelRatio();
        gl->glReadPixels(qRound(40 * dpr), qRound(40 * dpr), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        Require(pixel[0] >= 49 && pixel[0] <= 53 && pixel[1] >= 100 && pixel[1] <= 104
            && pixel[2] >= 202 && pixel[2] <= 206 && pixel[3] == 255,
            "Model pixels did not reach the window back buffer before swap");
        gl->glReadPixels(qRound(5 * dpr), qRound(5 * dpr), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        Require(pixel[3] == 0, "Window background must remain transparent after presentation blit");
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, previous);
        Require(gl->glGetError() == GL_NO_ERROR, "OpenGL error while checking window pixels");
        checkWindowPixels = false;
        ++presentedFrames;
    }
    static const auto swap = reinterpret_cast<decltype(&eglSwapBuffers)>(dlsym(RTLD_NEXT, "eglSwapBuffers"));
    Require(swap, "Missing real EGL swap function");
    return swap(display, surface);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    Events events;
    auto window = CreateDesktopWindow();
    const bool rejectPlatform = app.arguments().contains("--reject-platform");
    if (!window->Initialize(640, 480, events)) {
        window->Shutdown();
        return rejectPlatform ? 0 : 77;
    }
    Require(!rejectPlatform, "Non-Wayland platform must be rejected rather than fall back to a regular window");
    auto* qt = dynamic_cast<QWindow*>(window.get());
    Require(qt, "Expected a Qt window");
    pixelWindow = qt;
    const auto original = window->CurrentOutput();
    const auto outputs = window->Outputs();
    int frame = 0;
    int hotplugWait = 0;
    bool hotplugPassed = false;
    SlowExpose expose;
    qt->installEventFilter(&expose);
    bool eventSwitchPassed = outputs.size() < 2;
    if (outputs.size() > 1) QTimer::singleShot(0, [&] {
        expose.switching = true;
        Require(window->SwitchOutput(outputs[1].id), "Event-triggered output switch failed");
        Require(window->SwitchOutput(original), "Event-triggered switch back failed");
        expose.switching = false;
        Require(expose.delayed, "Missing expose delay for nested-frame regression");
        eventSwitchPassed = true;
    });
    const bool hotplug = qEnvironmentVariableIsSet("DESKWAIFU_QT_TEST_HOTPLUG") && outputs.size() > 1;
    const bool ran = window->Run([&] {
        Require(!expose.switching, "Frame callback ran during an event-triggered output rebind");
        Require(eventSwitchPassed, "Event-triggered output switch did not complete");
        Require(QOpenGLContext::currentContext()->format().profile() != QSurfaceFormat::CoreProfile,
            "Cubism client-side vertex arrays require an OpenGL compatibility context");
        auto* gl = QOpenGLContext::currentContext()->extraFunctions();
        Require(window->Framebuffer() != 0, "The model must draw into the backend FBO, not assume zero");
        Require(window->Width() > 0 && window->Height() > 0, "Missing logical size");
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glClearColor(0, 0, 0, 0);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        gl->glEnable(GL_SCISSOR_TEST);
        gl->glScissor(20, 20, 50, 50);
        gl->glClearColor(.2f, .4f, .8f, 1);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        gl->glDisable(GL_SCISSOR_TEST);
        unsigned char pixel[4] = {};
        gl->glReadPixels(25, 25, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        Require(pixel[3] == 255 && pixel[2] > 190, "GL content did not reach the model framebuffer");
        gl->glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        Require(pixel[3] == 0, "Background must remain transparent");
        switch (frame++) {
        case 0:
            Require(window->SetInputRegion({{10, 20, 30, 40}}), "Set model input");
            Require(qt->mask().contains(QPoint(15, 25)), "Nonempty model mask missing");
            Require(!qt->mask().contains(QPoint(0, 0)), "Background included in input mask");
            Require(!qt->flags().testFlag(Qt::WindowTransparentForInput), "Model should accept input");
            break;
        case 1:
            Require(window->SetInputRegion({}), "Clear input");
            Require(qt->flags().testFlag(Qt::WindowTransparentForInput), "Empty mask must explicitly disable all input");
            break;
        case 2: {
            window->SetInputRegion({{0, 0, window->Width(), window->Height()}});
            Require(!qt->flags().testFlag(Qt::WindowTransparentForInput), "Input did not recover after empty mask");
            QMouseEvent press(QEvent::MouseButtonDblClick, QPointF(25, 35), QPointF(25, 35),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(qt, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(25, 35), QPointF(25, 35),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(qt, &release);
            Require(events.presses == 1 && events.releases == 1 && events.x == 25 && events.y == 35,
                "Qt double-click must preserve the application's press/release policy and logical coordinates");
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, 23, 0, 0);
            QCoreApplication::sendEvent(qt, &key);
            Require(events.key == 15, "Qt native keycode must map to evdev");
            QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, 9, 0, 0, {}, true);
            QCoreApplication::sendEvent(qt, &repeat);
            Require(events.key == 15, "Auto-repeat should not trigger a model binding");
            const auto grab = window->SetKeyboardGrab(true);
            Require(window->KeyboardGrabbed() == (grab != ModelKeyboardGrabState::GrabResult::Rejected),
                "Keyboard interactivity must match the compositor's keyboard capability");
            QEvent focusOut(QEvent::FocusOut);
            QCoreApplication::sendEvent(qt, &focusOut);
            Require(!window->KeyboardGrabbed() && events.resets > 0, "Lost focus must release/reset keys");
            break;
        }
        case 3:
            Require(!window->SwitchOutput(0), "Invalid output must be rejected");
            Require(window->CurrentOutput() == original, "Invalid output changed selection");
            if (outputs.size() > 1) {
                Require(window->SwitchOutput(outputs[1].id), "Switch to second output failed");
                Require(window->CurrentOutput() == outputs[1].id, "Output identity not updated");
            }
            break;
        case 4:
            Require(window->SwitchOutput(original), "Switch back failed");
            break;
        case 5:
            if (!hotplug) return false;
            Require(QProcess::execute("kscreen-doctor", {QString("output.%1.disable").arg(
                QString::fromStdString(outputs[0].name))}) == 0, "Disable active output failed");
            break;
        case 6:
            if (window->Outputs().size() != 1 || window->CurrentOutput() == original) {
                Require(++hotplugWait < 100, "Active-output removal did not rebind to the remaining output");
                --frame;
                break;
            }
            Require(window->MakeCurrent(), "Output removal lost the original GPU context");
            Require(QProcess::execute("kscreen-doctor", {QString("output.%1.enable").arg(
                QString::fromStdString(outputs[0].name))}) == 0, "Re-enable removed output failed");
            break;
        case 7:
            if (window->Outputs().size() != 2) {
                Require(++hotplugWait < 200, "Output re-addition was not detected");
                --frame;
                break;
            }
            for (const auto& output : window->Outputs())
                if (output.name == outputs[0].name)
                    Require(output.id != original, "Re-added output must receive a fresh stable identity");
            hotplugPassed = true;
            return false;
        }
        // Reproduce NVIDIA/EGL's GL_NONE window draw target on every presented
        // frame, including after output replacement. Present must select it.
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, QOpenGLContext::currentContext()->defaultFramebufferObject());
        const GLenum none = GL_NONE;
        gl->glDrawBuffers(1, &none);
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, window->Framebuffer());
        checkWindowPixels = true;
        Require(gl->glGetError() == GL_NO_ERROR, "Unexpected OpenGL error");
        return true;
    });
    Require(ran && (frame == 6 || (frame == 8 && hotplugPassed)), "Qt event loop did not finish normally");
    Require(presentedFrames >= 5, "Window pixel checks did not run across output replacement");
    const qreal dpr = qt->devicePixelRatio();
    if (qEnvironmentVariableIsSet("DESKWAIFU_EXPECT_DPR"))
        Require(qAbs(dpr - qEnvironmentVariable("DESKWAIFU_EXPECT_DPR").toDouble()) < .01,
            "The compositor did not apply the requested real output scale");
    Require(window->MakeCurrent(), "GPU context must be restorable before teardown");
    Require(events.switchesStarted == events.switchesFinished,
        "Every output replacement must report completion");
    window->Shutdown();
    Require(!window->Available(), "Shutdown must release the Qt surface/context");
    std::cout << "Qt window rendering/input/output/teardown passed; outputs=" << outputs.size()
        << "; dpr=" << dpr << "; hotplug=" << hotplugPassed << "; presented=" << presentedFrames << '\n';
}
