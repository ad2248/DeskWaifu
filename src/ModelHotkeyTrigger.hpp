#pragma once

#include <string>

template <typename Hotkeys, typename Log>
bool TriggerModelHotkey(Hotkeys& hotkeys, const std::string& bindingId, Log log)
{
    std::string error;
    if (hotkeys.Trigger(bindingId, error)) return true;
    log(bindingId, error);
    return false;
}
