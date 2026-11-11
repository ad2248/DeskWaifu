#pragma once

// 记录逻辑键盘捕获状态，不负责向合成器发出焦点请求。
class ModelKeyboardGrabState {
public:
    // 区分有效变更、重复请求和缺少必要设备或表面而被拒绝的请求。
    enum class GrabResult { Changed, Unchanged, Rejected };

    // 仅申请捕获时要求表面和键盘均可用；释放不受可用性限制。
    GrabResult SetGrabbed(bool requested, bool hasSurface, bool hasKeyboard)
    {
        if (requested == _grabbed) {
            return GrabResult::Unchanged;
        }
        if (requested && (!hasSurface || !hasKeyboard)) {
            return GrabResult::Rejected;
        }
        _grabbed = requested;
        return GrabResult::Changed;
    }

    bool IsGrabbed() const { return _grabbed; }

    void Reset() { _grabbed = false; }

private:
    bool _grabbed = false;
};
