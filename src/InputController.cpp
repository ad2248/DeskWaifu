#include "InputController.hpp"
#include <cmath>

std::pair<float, float> InputController::LookAt(float x, float y, int width, int height,
    float centerX, float centerY, ModelTransform model)
{
    if (width <= 0 || height <= 0 || !std::isfinite(model.scale)) return {0.0f, 0.0f};
    const float faceX = width * centerX + model.x * model.scale * height * 0.5f;
    const float faceY = height * centerY - model.y * model.scale * height * 0.5f;
    return {(x - faceX) / (width * 0.5f), -(y - faceY) / (height * 0.5f)};
}

ModelTransform InputController::Drag(ModelTransform model, int dx, int dy, int viewportHeight)
{
    if (viewportHeight <= 0 || model.scale <= 0.0f || !std::isfinite(model.scale)) return model;
    const float unitsPerPixel = 2.0f / viewportHeight / model.scale;
    model.x += dx * unitsPerPixel;
    model.y -= dy * unitsPerPixel;
    return model;
}

bool InputController::IsTap(int startX, int startY, int endX, int endY)
{
    return std::abs(endX - startX) < 10 && std::abs(endY - startY) < 10;
}
