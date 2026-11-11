#pragma once

#include <cstdint>
#include <string>

#include "ModelKeyboard.hpp"

// 输入策略决策：保持现状、释放键盘捕获，或触发唯一匹配的绑定。
struct ModelKeyDecision {
    enum Action { None, ReleaseGrab, Trigger } action = None;
    std::string bindingId;
};

// 未捕获键盘时忽略输入；按下 Esc 时清空按键并请求释放捕获。
// 仅新按下事件且恰有一个有效绑定完全匹配时返回 Trigger。
template <typename Bindings> ModelKeyDecision DecideModelKey(ModelKeyboard& keyboard, bool grabbed, uint32_t code, bool pressed, const Bindings* bindings)
{
    if (!grabbed) {
        return { };
    }
    if (code == KEY_ESC && pressed) {
        keyboard.Reset();
        return { ModelKeyDecision::ReleaseGrab, { } };
    }
    if (!keyboard.Key(code, pressed) || !bindings) {
        return { };
    }

    // 与控制面板一致，忽略存在歧义的组合键。
    const typename Bindings::value_type* match = nullptr;
    for (const auto& binding : *bindings) {
        if (!binding.enabled || !binding.reason.empty() || !keyboard.Matches(binding.keys)) {
            continue;
        }
        if (match) {
            return { };
        }
        match = &binding;
    }
    if (!match) {
        return { };
    }
    return { ModelKeyDecision::Trigger, match->id };
}

// 取走待定单击并以释放坐标同步回调；没有待定点击时不调用 tap。
template <typename Tap> void DispatchPendingTap(ModelDoubleClick& clicks, Tap tap)
{
    if (clicks.Take()) {
        tap(clicks.X(), clicks.Y());
    }
}
