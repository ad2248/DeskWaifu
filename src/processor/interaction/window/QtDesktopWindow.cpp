#include "DesktopWindow.hpp"
#include "QtWindowInput.hpp"
#include "contracts/RenderFrame.hpp"
#include <QEventLoop>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_1_0>
#include <QOpenGLVersionFunctionsFactory>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWaylandClientExtension>
#include <wayland-util.h>
#include <QWindow>
#include <LayerShellQt/Window>
#include <iostream>

namespace {
// 查询 Qt 注册表只需要协议名称和版本。此扩展不绑定协议或发送请求，
// 实际协议实现由 LayerShellQt 持有。
class LayerShellProbe : public QWaylandClientExtension
{
public:
    LayerShellProbe() : QWaylandClientExtension(1) { initialize(); }
    const wl_interface* extensionInterface() const override
    {
        static const wl_interface interface = {"zwlr_layer_shell_v1", 1, 0, nullptr, 0, nullptr};
        return &interface;
    }
    void bind(wl_registry*, int, int) override {}
};

class QtDesktopWindow : public QWindow, public DesktopWindow
{
public:
    ~QtDesktopWindow() override
    {
        // 应用清理失败时，刻意将 GPU 对象留到进程退出时回收。
        (void)_frame.release();
        (void)_context.release();
    }
    bool Initialize(int width, int height, IDesktopWindowEvents& events) override
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
        // Cubism 现有渲染器使用客户端顶点和索引数组。
        // 否则 Qt 可能创建核心上下文，导致此类绘制无效。
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
        auto* gl = _context->extraFunctions();
        std::cout << "[GPU] Window presentation: " << gl->glGetString(GL_VENDOR) << " / "
            << gl->glGetString(GL_RENDERER) << " / " << gl->glGetString(GL_VERSION) << std::endl;
        std::cout << "[Qt] Layer-shell model window configured: " << Width() << 'x' << Height() << std::endl;
        return true;
    }

    bool Run(const std::function<bool()>& frame) override
    {
        bool ok = true;
        bool drawing = false;
        QEventLoop loop;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
            if (drawing || _switching) return; // 替换输出设备时会运行有时限的嵌套事件循环。
            drawing = true;
            if (_needsRebind) ReconcileOutputs();
            if (!MakeCurrent() || !BindFrame()) {
                ok = false;
                loop.quit();
            } else if (!frame()) {
                loop.quit();
            } else {
                if (!Present()) {
                    std::cerr << "[GPU] Failed to transfer/present the model frame\n";
                    ok = false;
                    loop.quit();
                }
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
            else (void)_frame.release(); // 缺少上下文时不能执行 GL 析构。
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
    unsigned int Framebuffer() const override
    {
        return _frame ? _frame->handle() : 0;
    }
    bool UploadFrame(const RenderFrame& frame) override
    {
        if (frame.width <= 0 || frame.height <= 0 || frame.rgba.size() != size_t(frame.width) * frame.height * 4) return false;
        if (!_frame || _frame->width() != frame.width || _frame->height() != frame.height)
            _frame.reset(new QOpenGLFramebufferObject(frame.width, frame.height));
        if (!_frame->isValid()) return false;
        auto* gl = _context->extraFunctions();
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1); gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        gl->glActiveTexture(GL_TEXTURE0); gl->glBindTexture(GL_TEXTURE_2D, _frame->texture());
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        return gl->glGetError() == GL_NO_ERROR;
    }
    bool SetInputRegion(const std::vector<ModelRegion>& regions) override
    {
        if (!Available()) return false;
        const QRegion mask = QtModelInputRegion(regions);
        _emptyInput = mask.isEmpty();
        // 空 QWindow 掩码表示恢复整个窗口，而不是不接收输入。
        // 移除掩码前，先显式设置全部输入穿透标志。
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
    ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool grabbed) override
    {
        // 首次按键事件之前，Qt 设备列表可能还没有键盘。
        // 应让合成器授予 layer-shell 焦点，不能拒绝接收首次事件所必需的请求。
        const auto result = _keyboard.SetGrabbed(grabbed, Available(), true);
        if (result != ModelKeyboardGrabState::GrabResult::Changed) return result;
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
        if (_switching) return false;
        QPointer<QScreen> previous = screen();
        SetKeyboardGrab(false);
        if (_events) _events->PointerLeft();
        _switching = true;
        if (_events) _events->OutputSwitchStarted();
        _context->doneCurrent();
        hide();
        destroy();
        setScreen(chosen);
        LayerShellQt::Window::get(this)->setScreen(chosen);
        show();
        if (!WaitForExpose() || !_context->makeCurrent(this) || !MakeCurrent()) {
            bool restored = false;
            if (previous) {
                hide();
                destroy();
                setScreen(previous);
                LayerShellQt::Window::get(this)->setScreen(previous);
                show();
                restored = WaitForExpose() && _context->makeCurrent(this) && MakeCurrent();
            }
            if (!restored && _events) _events->SurfaceClosed();
            _switching = false;
            if (_events) _events->OutputSwitchFinished();
            return false;
        }
        _current = id;
        _needsRebind = false;
        _switching = false;
        if (_events) _events->OutputSwitchFinished();
        std::cout << "[Qt] Output switch configured and committed: " << id << std::endl;
        return true;
    }

protected:
    bool event(QEvent* event) override
    {
        if (_events && event->type() == QEvent::Close) {
            _events->SurfaceClosed();
            event->ignore(); // 保留表面，以便按顺序清理 GPU 资源。
            return true;
        }
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
                if (SetKeyboardGrab(false) != ModelKeyboardGrabState::GrabResult::Changed)
                    _events->KeyboardChanged();
                break;
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
        // 在任意输出缩放比例下保留现有逻辑像素裁剪和配置语义。
        // 只有最终呈现时才缩放到物理后备缓冲区。
        if (!_frame || _frame->size() != size()) {
            _frame.reset(new QOpenGLFramebufferObject(size(), QOpenGLFramebufferObject::CombinedDepthStencil));
        }
        return _frame->isValid() && _frame->bind();
    }
    bool Present()
    {
        const int physicalWidth = qRound(width() * devicePixelRatio());
        const int physicalHeight = qRound(height() * devicePixelRatio());
        auto* gl = _context->extraFunctions();
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, _frame->handle());
        const auto destination = _context->defaultFramebufferObject();
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);
        const bool scissor = gl->glIsEnabled(GL_SCISSOR_TEST);
        gl->glDisable(GL_SCISSOR_TEST);
        // NVIDIA/EGL 可能让窗口绘制目标停留在 GL_NONE。
        // 仅绑定帧缓冲不会恢复绘制目标，随后执行复制也不会显示内容。
        auto* desktopGl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_1_0>(_context.get());
        desktopGl->glDrawBuffer(destination ? GL_COLOR_ATTACHMENT0 : GL_BACK);
        gl->glBlitFramebuffer(0, 0, _frame->width(), _frame->height(), 0, 0,
            physicalWidth, physicalHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        const bool presented = gl->glGetError() == GL_NO_ERROR;
        if (scissor) gl->glEnable(GL_SCISSOR_TEST);
        _context->swapBuffers(this);
        _frame->bind();
        return presented;
    }
    struct ScreenEntry { OutputId id; QPointer<QScreen> screen; };
    std::vector<ScreenEntry> _screens;
    OutputId _current = 0, _nextId = 1;
    bool _needsRebind = false;
    bool _emptyInput = true;
    bool _switching = false;
    IDesktopWindowEvents* _events = nullptr;
    ModelKeyboardGrabState _keyboard;
    std::unique_ptr<QOpenGLContext> _context;
    std::unique_ptr<QOpenGLFramebufferObject> _frame;
};
}

std::unique_ptr<DesktopWindow> CreateDesktopWindow()
{
    return std::unique_ptr<DesktopWindow>(new QtDesktopWindow());
}
