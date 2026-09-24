#pragma once

// Releasing a stale grab must work even after the layer surface or seat vanishes.
inline bool CanChangeKeyboardGrab(bool current, bool requested,
    bool hasSurface, bool hasKeyboard)
{
    return current != requested && (!requested || (hasSurface && hasKeyboard));
}
