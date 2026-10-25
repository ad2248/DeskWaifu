#pragma once

#include <cstdint>
#include <string>

#include "ModelKeyboard.hpp"

struct ModelKeyDecision
{
    enum Action { None, ReleaseGrab, Trigger } action = None;
    std::string bindingId;
};

template <typename Bindings>
ModelKeyDecision DecideModelKey(ModelKeyboard& keyboard, bool grabbed,
    uint32_t code, bool pressed, const Bindings* bindings)
{
    if (!grabbed) return {};
    if (code == KEY_ESC && pressed)
    {
        keyboard.Reset();
        return {ModelKeyDecision::ReleaseGrab, {}};
    }
    if (!keyboard.Key(code, pressed) || !bindings) return {};

    // 与控制面板一致，忽略存在歧义的组合键。
    const typename Bindings::value_type* match = nullptr;
    for (const auto& binding : *bindings)
    {
        if (!binding.enabled || !binding.reason.empty() || !keyboard.Matches(binding.keys)) continue;
        if (match) return {};
        match = &binding;
    }
    if (!match) return {};
    return {ModelKeyDecision::Trigger, match->id};
}

template <typename Tap>
void DispatchPendingTap(ModelDoubleClick& clicks, Tap tap)
{
    if (clicks.Take()) tap(clicks.X(), clicks.Y());
}
