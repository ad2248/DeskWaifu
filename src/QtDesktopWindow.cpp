#include "DesktopWindow.hpp"
#include "QtWindowInput.hpp"
#include <QEventLoop>
#include <QGuiApplication>
#include <QInputDevice>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWaylandClientExtension>
#include <QWindow>
#include <LayerShellQt/Window>
#include <algorithm>
#include <iostream>

#define namespace wayland_namespace
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

namespace {
// Probe via Qt's registry, without owning a second Wayland connection or shell.
class LayerShellProbe : public QWaylandClientExtension
{
public:
    LayerShellProbe() : QWaylandClientExtension(1) { initialize(); }
    const wl_interface* extensionInterface() const override { return &zwlr_layer_shell_v1_interface; }
    void bind(wl_registry*, int, int) override {}
};

class QtDesktopWindow : public QWindow, public DesktopWindow
{
public:
    ~QtDesktopWindow() override
    {
        // Failed application teardown intentionally leaves GPU objects to process exit.
        (void)_frame.release();
        (void)_context.release();
    }
    bool Initialize(int width, int height, IWaylandEvents& events) override
    {
        if (!QGuiApplication::platformName().startsWith("wayland")) {
            std::cerr << "[Qt] The model window requires the Qt Wayland platform plugin\n";
            return false;
        }
        LayerShellProbe probe;
        if (!probe.isActive()) {
            std::cerr << "[Qt] The compositor does not advertise wlr-layer-shell\n";
            return false;
        }
        _events = &events;
        setSurfaceType(QSurface::OpenGLSurface);
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        // Cubism's existing renderer uses client-side vertex/index arrays.
        // Qt may otherwise create a core context, where those draws are invalid.
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        format.setOption(QSurfaceFormat::DeprecatedFunctions);
        format.setAlphaBufferSize(8);
        format.setDepthBufferSize(24);
        format.setStencilBufferSize(8);
        format.setSwapInterval(1);
        setFormat(format);
        setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput);
        resize(width, height);
        auto* layer = LayerShellQt::Window::get(this);
        layer->setScope("deskwaifu");
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorBottom
            | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setActivateOnShow(false);
        layer->setCloseOnDismissed(false);
        for (auto* screen : QGuiApplication::screens()) AddScreen(screen);
        if (_screens.empty()) return false;
        _current = _screens.front().id;
        layer->setScreen(_screens.front().screen);
        setScreen(_screens.front().screen);
        QObject::connect(qGuiApp, &QGuiApplication::screenAdded, this,
            [this](QScreen* screen) { AddScreen(screen); });
        QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
            for (auto it = _screens.begin(); it != _screens.end(); ++it)
                if (it->screen == screen) {
                    if (it->id == _current) _needsRebind = true;
                    _screens.erase(it);
                    break;
                }
        });
        show();
        if (!WaitForExpose()) return false;
        _context.reset(new QOpenGLContext());
        _context->setFormat(requestedFormat());
        if (!_context->create() || !MakeCurrent()) {
            std::cerr << "[Qt] Failed to create/bind the model OpenGL context\n";
            return false;
        }
        if (_context->isOpenGLES() || _context->format().version() < qMakePair(3, 3)
            || _context->format().profile() == QSurfaceFormat::CoreProfile) {
            std::cerr << "[Qt] Cubism requires a desktop OpenGL 3.3 compatibility context\n";
            return false;
        }
        std::cout << "[Qt] Layer-shell model window configured: " << Width() << 'x' << Height() << '\n';
        return true;
    }

    bool Run(const std::function<bool()>& frame) override
    {
        bool ok = true;
        bool drawing = false;
        QEventLoop loop;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
            if (drawing) return; // Output replacement runs a bounded nested event loop.
            drawing = true;
            if (_needsRebind) ReconcileOutputs();
            if (!MakeCurrent() || !BindFrame()) {
                ok = false;
                loop.quit();
            } else if (!frame()) {
                loop.quit();
            } else {
                Present();
                timer.setInterval(_emptyInput ? 33 : 16);
            }
            drawing = false;
        });
        timer.start(16);
        loop.exec();
        return ok;
    }

    bool MakeCurrent() override
    {
        return _context && handle() && _context->makeCurrent(this)
            && QOpenGLContext::currentContext() == _context.get();
    }
    void Shutdown() override
    {
        _events = nullptr;
        if (_context) {
            if (MakeCurrent()) _frame.reset();
            else (void)_frame.release(); // Never call a GL destructor on a missing context.
            _context->doneCurrent();
            _context.reset();
        }
        hide();
        destroy();
        _keyboard.Reset();
    }
    bool Available() const override { return handle() && _context && _context->isValid(); }
    int Width() const override { return width(); }
    int Height() const override { return height(); }
    unsigned int Framebuffer() const override { return _frame ? _frame->handle() : 0; }
    bool SetInputRegion(const std::vector<ModelRegion>& regions) override
    {
        if (!Available()) return false;
        const QRegion mask = QtModelInputRegion(regions);
        _emptyInput = mask.isEmpty();
        // An empty QWindow mask means "reset to the whole window", NOT no input.
        // Set the explicit all-input-transparent flag before removing the mask.
        if (mask.isEmpty()) {
            setFlag(Qt::WindowTransparentForInput, true);
            setMask({});
        } else {
            setMask(mask);
            setFlag(Qt::WindowTransparentForInput, false);
        }
        return true;
    }
    bool KeyboardGrabbed() const override { return _keyboard.IsGrabbed(); }
    KeyboardSurfaceLifecycle::GrabResult SetKeyboardGrab(bool grabbed) override
    {
        const auto devices = QInputDevice::devices();
        const bool hasKeyboard = std::any_of(devices.begin(), devices.end(), [](const QInputDevice* device) {
            return device->type() == QInputDevice::DeviceType::Keyboard;
        });
        const auto result = _keyboard.SetGrabbed(grabbed, Available(), hasKeyboard);
        if (result != KeyboardSurfaceLifecycle::GrabResult::Changed) return result;
        LayerShellQt::Window::get(this)->setKeyboardInteractivity(grabbed
            ? LayerShellQt::Window::KeyboardInteractivityExclusive
            : LayerShellQt::Window::KeyboardInteractivityNone);
        if (!grabbed && _events) _events->KeyboardChanged();
        return result;
    }
    bool OutputNeedsRebind() const override { return _needsRebind; }
    void ReconcileOutputs() override
    {
        if (!_needsRebind) return;
        if (_screens.empty() || !SwitchOutput(_screens.front().id))
            if (_events) _events->SurfaceClosed();
    }
    // QScreen already exposes logical geometry; do not overwrite it with native modes.
    void ApplyMonitorGeometry(const std::vector<OutputSnapshot>&) override {}
    std::vector<OutputSnapshot> Outputs() const override
    {
        std::vector<OutputSnapshot> outputs;
        for (const auto& entry : _screens) {
            if (!entry.screen) continue;
            const QRect geometry = entry.screen->geometry();
            outputs.push_back({entry.id, entry.screen->name().toStdString(), geometry.x(),
                geometry.y(), geometry.width(), geometry.height()});
        }
        return outputs;
    }
    OutputId CurrentOutput() const override { return _current; }
    bool SwitchOutput(OutputId id) override
    {
        if (id == _current && !_needsRebind) return true;
        QPointer<QScreen> chosen;
        for (const auto& entry : _screens) if (entry.id == id) chosen = entry.screen;
        if (!chosen || !_context) return false;
        QPointer<QScreen> previous = screen();
        const bool grabbed = KeyboardGrabbed();
        SetKeyboardGrab(false);
        if (_events) _events->PointerLeft();
        _switching = true;
        _context->doneCurrent();
        hide();
        destroy();
        setScreen(chosen);
        LayerShellQt::Window::get(this)->setScreen(chosen);
        show();
        if (!WaitForExpose() || !MakeCurrent()) {
            if (previous) {
                hide();
                destroy();
                setScreen(previous);
                LayerShellQt::Window::get(this)->setScreen(previous);
                show();
                if (!WaitForExpose() || !MakeCurrent())
                    if (_events) _events->SurfaceClosed();
            }
            _switching = false;
            return false;
        }
        _current = id;
        _needsRebind = false;
        _switching = false;
        SetKeyboardGrab(grabbed);
        std::cout << "[Qt] Output switch configured and committed: " << id << '\n';
        return true;
    }

protected:
    bool event(QEvent* event) override
    {
        if (_events && !_switching) {
            switch (event->type()) {
            case QEvent::Enter: {
                auto* enter = static_cast<QEnterEvent*>(event);
                _events->PointerEntered(enter->position().x(), enter->position().y());
                break;
            }
            case QEvent::Leave: _events->PointerLeft(); break;
            case QEvent::MouseMove: {
                auto* mouse = static_cast<QMouseEvent*>(event);
                _events->PointerMoved(mouse->position().x(), mouse->position().y());
                break;
            }
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseButtonRelease: {
                auto* mouse = static_cast<QMouseEvent*>(event);
                const int button = mouse->button() == Qt::LeftButton ? 0
                    : mouse->button() == Qt::RightButton ? 1
                    : mouse->button() == Qt::MiddleButton ? 2 : -1;
                _events->PointerMoved(mouse->position().x(), mouse->position().y());
                if (button >= 0) _events->PointerButton(button, event->type() != QEvent::MouseButtonRelease);
                break;
            }
            case QEvent::Wheel: {
                auto* wheel = static_cast<QWheelEvent*>(event);
                const int delta = wheel->angleDelta().y();
                if (delta) _events->PointerScroll(delta / 120.0);
                break;
            }
            case QEvent::KeyPress:
            case QEvent::KeyRelease: {
                auto* key = static_cast<QKeyEvent*>(event);
                if (!key->isAutoRepeat() && key->nativeScanCode() >= 8)
                    _events->KeyboardKey(QtWaylandModelKey(key->nativeScanCode()), event->type() == QEvent::KeyPress);
                break;
            }
            case QEvent::FocusIn: _events->KeyboardChanged(); break;
            case QEvent::FocusOut:
                if (SetKeyboardGrab(false) != KeyboardSurfaceLifecycle::GrabResult::Changed)
                    _events->KeyboardChanged();
                break;
            case QEvent::Close:
                _events->SurfaceClosed();
                event->ignore(); // Keep the surface alive for ordered GPU teardown.
                return true;
            default: break;
            }
        }
        return QWindow::event(event);
    }

private:
    bool WaitForExpose()
    {
        QEventLoop loop;
        QTimer poll, timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (isExposed()) loop.quit(); });
        poll.start(10);
        timeout.start(3000);
        if (!isExposed()) loop.exec();
        return isExposed() && width() > 0 && height() > 0;
    }
    void AddScreen(QScreen* screen) { _screens.push_back({_nextId++, screen}); }
    bool BindFrame()
    {
        // Retain existing logical-pixel crop/config semantics at any output scale.
        // Only final presentation scales to the physical backing buffer.
        if (!_frame || _frame->size() != size()) {
            _frame.reset(new QOpenGLFramebufferObject(size(), QOpenGLFramebufferObject::CombinedDepthStencil));
        }
        return _frame->isValid() && _frame->bind();
    }
    void Present()
    {
        const int physicalWidth = qRound(width() * devicePixelRatio());
        const int physicalHeight = qRound(height() * devicePixelRatio());
        auto* gl = _context->extraFunctions();
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, _frame->handle());
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _context->defaultFramebufferObject());
        gl->glBlitFramebuffer(0, 0, _frame->width(), _frame->height(), 0, 0,
            physicalWidth, physicalHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        _context->swapBuffers(this);
        _frame->bind(); // Input/IPC callbacks may use model GPU resources between frames.
    }
    struct ScreenEntry { OutputId id; QPointer<QScreen> screen; };
    std::vector<ScreenEntry> _screens;
    OutputId _current = 0, _nextId = 1;
    bool _needsRebind = false;
    bool _emptyInput = true;
    bool _switching = false;
    IWaylandEvents* _events = nullptr;
    KeyboardSurfaceLifecycle _keyboard;
    std::unique_ptr<QOpenGLContext> _context;
    std::unique_ptr<QOpenGLFramebufferObject> _frame;
};
}

std::unique_ptr<DesktopWindow> CreateDesktopWindow()
{
    return std::unique_ptr<DesktopWindow>(new QtDesktopWindow());
}
