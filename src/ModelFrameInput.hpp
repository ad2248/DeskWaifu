#pragma once

// Immutable value handed to a model once per frame. Acquisition of input is
// the composition root's responsibility; the model has no transport dependency.
struct ModelFrameInput
{
    bool hasMouth = false;
    float mouth = 0.0f;
    bool hasLook = false;
    float lookX = 0.0f;
    float lookY = 0.0f;
};

template<typename MouthSink, typename LookSink>
void DispatchModelFrameInput(const ModelFrameInput& input, MouthSink mouth, LookSink look)
{
    if (input.hasMouth) mouth(input.mouth);
    if (input.hasLook) look(input.lookX, input.lookY);
}
