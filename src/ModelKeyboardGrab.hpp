#pragma once

// Releasing a stale grab must work even after the layer surface or seat vanishes.
inline bool CanChangeKeyboardGrab(bool current, bool requested,
    bool hasSurface, bool hasKeyboard)
{
    return current != requested && (!requested || (hasSurface && hasKeyboard));
}

inline bool UpdateKeyboardGrabState(bool& current, bool requested,
    bool hasSurface, bool hasKeyboard)
{
    if (!CanChangeKeyboardGrab(current, requested, hasSurface, hasKeyboard)) return false;
    current = requested;
    return true;
}

inline bool DeferKeyboardInteractivity(bool configured, bool& dirty)
{
    if (configured) return false;
    dirty = true;
    return true;
}

inline bool TakePendingKeyboardInteractivity(bool& dirty)
{
    if (!dirty) return false;
    dirty = false;
    return true;
}
