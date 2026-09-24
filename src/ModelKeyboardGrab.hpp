#pragma once

class KeyboardSurfaceLifecycle
{
public:
    enum class GrabResult { Changed, Unchanged, Rejected };

    GrabResult SetGrabbed(bool requested, bool hasSurface, bool hasKeyboard)
    {
        if (requested == _grabbed) return GrabResult::Unchanged;
        if (requested && (!hasSurface || !hasKeyboard)) return GrabResult::Rejected;
        _grabbed = requested;
        return GrabResult::Changed;
    }

    bool IsGrabbed() const { return _grabbed; }
    bool IsConfigured() const { return _configured; }

    // A replacement layer surface must wait for its first configure.
    void BeginSurface() { _configured = false; _interactivityDirty = false; }

    // Returns true when the caller must defer protocol changes until configure.
    bool RequestInteractivityUpdate()
    {
        if (_configured) return false;
        _interactivityDirty = true;
        return true;
    }

    // Called for every configure; only the first configure after a deferred
    // state change asks the caller to apply and commit keyboard interactivity.
    bool OnConfigure()
    {
        _configured = true;
        const bool apply = _interactivityDirty;
        _interactivityDirty = false;
        return apply;
    }

    void Reset()
    {
        _grabbed = false;
        _configured = false;
        _interactivityDirty = false;
    }

private:
    bool _grabbed = false;
    bool _configured = false;
    bool _interactivityDirty = false;
};
