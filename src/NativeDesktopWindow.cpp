#include "DesktopWindow.hpp"
#include "LAppWayland.hpp"
#include "LAppWaylandRegion.hpp"
#include <unistd.h>

namespace {
class NativeDesktopWindow : public DesktopWindow
{
public:
    NativeDesktopWindow() : _outputs(_context) {}
    bool Initialize(int width, int height, IWaylandEvents& events) override
    {
        _context.events = &events;
        return SetupWaylandContext(&_context, width, height);
    }
    bool Run(const std::function<bool()>& frame) override
    {
        while (true) {
            if (wl_display_dispatch_pending(_context.display) == -1) return false;
            wl_display_flush(_context.display);
            if (!frame()) return true;
            if (!eglSwapBuffers(_context.egl_display, _context.egl_surface)) return false;
            if (_emptyInput) usleep(33000);
        }
    }
    bool MakeCurrent() override { return MakeWaylandGpuContextCurrent(&_context); }
    void Shutdown() override { CleanWaylandContext(&_context); }
    bool Available() const override { return _context.surface && _context.compositor; }
    int Width() const override { return _context.width; }
    int Height() const override { return _context.height; }
    unsigned int Framebuffer() const override { return 0; }
    bool SetInputRegion(const std::vector<ModelRegion>& regions) override
    {
        if (!Available()) return false;
        _emptyInput = regions.empty();
        if (_emptyInput) return ClearWaylandInputRegion(&_context);
        UpdateWaylandInputRegion(&_context, regions);
        return true;
    }
    bool KeyboardGrabbed() const override { return _context.keyboardSurface.IsGrabbed(); }
    KeyboardSurfaceLifecycle::GrabResult SetKeyboardGrab(bool grabbed) override
    { return SetModelKeyboardGrab(&_context, grabbed); }
    bool OutputNeedsRebind() const override { return _context.outputNeedsRebind; }
    void ReconcileOutputs() override { _outputs.ReconcileOutputs(); }
    void ApplyMonitorGeometry(const std::vector<OutputSnapshot>& monitors) override
    { _outputs.ApplyMonitorGeometry(monitors); }
    std::vector<OutputSnapshot> Outputs() const override { return _outputs.Outputs(); }
    OutputId CurrentOutput() const override { return _outputs.CurrentOutput(); }
    bool SwitchOutput(OutputId id) override { return _outputs.SwitchOutput(id); }
private:
    WaylandContext _context;
    WaylandBackend _outputs;
    bool _emptyInput = false;
};
}

std::unique_ptr<DesktopWindow> CreateDesktopWindow()
{
    return std::unique_ptr<DesktopWindow>(new NativeDesktopWindow());
}
