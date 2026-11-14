#pragma once

#include "ModelInputRegion.hpp"
#include <QMetaType>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// 模型空间中的具名轴对齐命中矩形。
struct ModelHitArea {
    std::string name;
    float left = 0, top = 0, right = 0, bottom = 0;
};

// 输入像素坐标及其视口、模型令牌，用于拒绝过期快照。
struct ModelHitQuery {
    double x = 0, y = 0;
    int width = 0, height = 0;
    std::string modelToken;
};

// available 区分过期或缺失快照；inside 是像素区域命中，parts 是模型部位命中。
struct ModelHitResult {
    bool available = false, inside = false;
    std::vector<std::string> parts;
};

// 只包含 CPU 值；坐标转换与 Cubism 的包围矩形命中算法一致。
struct ModelHitSnapshot {
    int width = 0, height = 0;
    std::string modelToken;
    bool hidden = false, partsEnabled = false, fallback = false;
    float xScale = 0, xOffset = 0, yScale = 0, yOffset = 0;
    float fallbackYScale = 0, fallbackYOffset = 0;
    std::vector<ModelRegion> regions;
    std::vector<ModelHitArea> areas;

    // 视口或令牌不匹配时返回不可用结果；隐藏模型返回可用但不命中的结果。
    ModelHitResult Test(const ModelHitQuery& query) const
    {
        if (query.width != width || query.height != height || query.modelToken != modelToken) {
            return { };
        }
        ModelHitResult result;
        result.available = true;
        if (hidden) {
            return result;
        }
        result.inside = ModelInputRegionContains(regions, query.x, query.y);
        if (!partsEnabled) {
            return result;
        }
        if (fallback) {
            result.parts.push_back(query.y * fallbackYScale + fallbackYOffset > 0 ? "Head" : "Body");
        } else {
            const double x = query.x * xScale + xOffset, y = query.y * yScale + yOffset;
            for (const auto& area : areas) {
                if (x >= area.left && x <= area.right && y >= area.top && y <= area.bottom) {
                    result.parts.push_back(area.name);
                }
            }
        }
        return result;
    }
};

using ModelHitTest = std::function<ModelHitResult(const ModelHitQuery&)>;
Q_DECLARE_METATYPE(ModelHitTest)

// 回调只弱引用交换槽，不借用处理器、SDK 对象或 GL 上下文。
struct ModelHitMailbox {
    std::mutex mutex;
    bool active = true;
    std::shared_ptr<const ModelHitSnapshot> snapshot;
    // 返回可跨线程调用的加锁查询；邮箱失效或停用时返回不可用结果。
    ModelHitTest Callback(const std::shared_ptr<ModelHitMailbox>& self)
    {
        return [weak = std::weak_ptr<ModelHitMailbox>(self)](const ModelHitQuery& query) {
            const auto box = weak.lock();
            if (!box) {
                return ModelHitResult { };
            }
            std::lock_guard<std::mutex> lock(box->mutex);
            return box->active && box->snapshot ? box->snapshot->Test(query) : ModelHitResult { };
        };
    }
};
