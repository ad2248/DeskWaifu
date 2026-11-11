// 定义模型动作和表情绑定的跨模块数据契约。
#pragma once
#include <string>
#include <vector>

// 描述模型动作或表情与触发按键的绑定，以及可用性和播放参数。
struct ModelBinding {
    std::string id, name, action, file, reason;
    std::vector<std::string> keys;
    bool enabled = true;
    // 淡变与自动停用时长均以秒计，duration 为负表示不自动停用。
    float fade = 0.5f, duration = -1;
    // 当前加载配置中解析出的表情索引；-1 表示未解析到有效表情。
    int expression = -1;
};
