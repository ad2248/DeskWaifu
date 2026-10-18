#pragma once

class ModelKeyboardGrabState
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

    void Reset()
    {
        _grabbed = false;
    }

private:
    bool _grabbed = false;
};
