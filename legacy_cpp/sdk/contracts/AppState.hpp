// 定义应用命令结果及供外部查询的模型状态快照。
#pragma once
#include "ModelTransform.hpp"
#include <string>

// 表示应用操作是否成功；失败时携带可向调用者报告的错误信息。
struct AppResult {
    bool ok;
    std::string error;
    static AppResult Success() { return { true, { } }; }
    static AppResult Failure(std::string message) { return { false, message }; }
};

// 应用当前模型的显示状态；modelIndex 为 -1 表示没有选中的模型。
struct AppSnapshot {
    ModelTransform transform { 1.0f, 0.0f, 0.0f };
    bool hidden = false;
    int modelIndex = -1;
    int modelCount = 0;
};
