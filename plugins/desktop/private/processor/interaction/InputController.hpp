#pragma once

#include "contracts/ModelTransform.hpp"
#include "processor/interaction/ModelInputDispatch.hpp"
#include <utility>

// 纯粹的指针到模型交互策略，不依赖 Wayland 或 Delegate。
class InputController
{
public:
    enum class ReleaseResult { Ignored, Drag, TapPending, DoubleClick };

    void Press(int x, int y)
    {
        _captured = true;
        _startX = _dragX = x;
        _startY = _dragY = y;
    }
    bool IsCaptured() const { return _captured; }
    int StartX() const { return _startX; }
    int StartY() const { return _startY; }
    int DragX() const { return _dragX; }
    int DragY() const { return _dragY; }
    void SetDragPosition(int x, int y) { _dragX = x; _dragY = y; }
    bool TapExpired(uint32_t now) const { return _clicks.Expired(now); }

    template<typename Tap>
    void FlushTap(Tap tap) { DispatchPendingTap(_clicks, tap); }

    template<typename Tap>
    ReleaseResult Release(uint32_t now, int x, int y, Tap tap)
    {
        if (!_captured) return ReleaseResult::Ignored;
        _captured = false;
        if (!IsTap(_startX, _startY, x, y))
        {
            FlushTap(tap);
            return ReleaseResult::Drag;
        }
        if (_clicks.HasPending() && !_clicks.IsSecondClick(now, _startX, _startY))
            FlushTap(tap);
        return _clicks.Release(now, _startX, _startY, x, y) == ModelDoubleClick::DoubleClick
            ? ReleaseResult::DoubleClick : ReleaseResult::TapPending;
    }

    void ResetKeyboard() { _keyboard.Reset(); }
    template<typename Bindings>
    ModelKeyDecision Key(bool grabbed, uint32_t code, bool pressed, const Bindings* bindings)
    {
        return DecideModelKey(_keyboard, grabbed, code, pressed, bindings);
    }

    static std::pair<float, float> LookAt(float x, float y, int width, int height,
        float centerX, float centerY, ModelTransform model);
    static ModelTransform Drag(ModelTransform model, int dx, int dy, int viewportHeight);
    static bool IsTap(int startX, int startY, int endX, int endY);

private:
    bool _captured = false;
    int _startX = 0, _startY = 0;
    int _dragX = 0, _dragY = 0;
    ModelDoubleClick _clicks;
    ModelKeyboard _keyboard;
};
