#pragma once
#include "ModelTransform.hpp"
#include <string>

struct AppResult
{
    bool ok;
    std::string error;
    static AppResult Success() { return {true, {}}; }
    static AppResult Failure(std::string message) { return {false, message}; }
};

struct AppSnapshot
{
    ModelTransform transform{1.0f, 0.0f, 0.0f};
    bool hidden = false;
    int modelIndex = -1;
    int modelCount = 0;
};
