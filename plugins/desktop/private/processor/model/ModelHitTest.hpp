#pragma once

#include "ModelInputRegion.hpp"
#include <QMetaType>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct ModelHitArea
{
    std::string name;
    float left = 0, top = 0, right = 0, bottom = 0;
};

struct ModelHitQuery
{
    double x = 0, y = 0;
    int width = 0, height = 0;
    std::string modelToken;
};

struct ModelHitResult
{
    bool available = false, inside = false;
    std::vector<std::string> parts;
};

// 只包含 CPU 值；坐标转换与 Cubism 的包围矩形命中算法一致。
struct ModelHitSnapshot
{
    int width = 0, height = 0;
    std::string modelToken;
    bool hidden = false, partsEnabled = false, fallback = false;
    float xScale = 0, xOffset = 0, yScale = 0, yOffset = 0;
    float fallbackYScale = 0, fallbackYOffset = 0;
    std::vector<ModelRegion> regions;
    std::vector<ModelHitArea> areas;

    ModelHitResult Test(const ModelHitQuery& query) const
    {
        if (query.width != width || query.height != height || query.modelToken != modelToken) return {};
        ModelHitResult result; result.available = true;
        if (hidden) return result;
        result.inside = ModelInputRegionContains(regions, query.x, query.y);
        if (!partsEnabled) return result;
        if (fallback) {
            result.parts.push_back(query.y * fallbackYScale + fallbackYOffset > 0 ? "Head" : "Body");
        } else {
            const double x = query.x * xScale + xOffset, y = query.y * yScale + yOffset;
            for (const auto& area : areas)
                if (x >= area.left && x <= area.right && y >= area.top && y <= area.bottom)
                    result.parts.push_back(area.name);
        }
        return result;
    }
};

using ModelHitTest = std::function<ModelHitResult(const ModelHitQuery&)>;
Q_DECLARE_METATYPE(ModelHitTest)

// 回调只弱引用交换槽，不借用处理器、SDK 对象或 GL 上下文。
struct ModelHitMailbox
{
    std::mutex mutex;
    bool active = true;
    std::shared_ptr<const ModelHitSnapshot> snapshot;
    ModelHitTest Callback(const std::shared_ptr<ModelHitMailbox>& self)
    {
        return [weak = std::weak_ptr<ModelHitMailbox>(self)](const ModelHitQuery& query) {
            const auto box = weak.lock();
            if (!box) return ModelHitResult{};
            std::lock_guard<std::mutex> lock(box->mutex);
            return box->active && box->snapshot ? box->snapshot->Test(query) : ModelHitResult{};
        };
    }
};
