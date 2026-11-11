#pragma once

#include "contracts/ModelBinding.hpp"
#include <string>
#include <vector>

namespace Live2D {
namespace Cubism {
    namespace Framework {
        class CubismModel;
    }
}
}

// 与渲染器独立的模型配置和持久表情状态。
class ModelHotkeys {
public:
    // 单个表情参数及其叠加、乘算或覆盖混合方式。
    struct Parameter {
        std::string id, blend;
        float value;
    };
    struct Expression {
        std::string file;
        std::vector<Parameter> parameters;
        bool active = false;
        float weight = 0, fade = 0.5f, remaining = -1;
    };
    using Binding = ModelBinding;

    // 重新读取模型热键和表情配置，并生成本次加载的模型令牌；警告由 Error() 提供。
    void Load(const std::string& directory, const std::string& modelFile);
    // 清除已激活表情及其混合权重，不卸载配置；更新令牌以拒绝旧操作。
    void Reset();
    // 按绑定 ID 切换表情；不可用时返回 false，并填写 error。
    bool Trigger(const std::string& id, std::string& error);
    // 按秒推进淡入淡出及表情超时。
    void Update(float seconds);
    // 将当前权重应用到借用模型的参数；不保存模型指针。
    void Apply(Live2D::Cubism::Framework::CubismModel* model) const;
    // 根据当前模型的绑定解析 ID，不使用调用方传入的索引。
    bool IsActive(const std::string& id) const;
    const std::vector<Binding>& Bindings() const { return _bindings; }
    const std::vector<Expression>& Expressions() const { return _expressions; }
    const std::string& Error() const { return _error; }
    const std::string& Token() const { return _token; }
    static std::vector<std::string> ExpressionDirectories();

private:
    std::vector<Binding> _bindings;
    std::vector<Expression> _expressions;
    std::string _error;
    std::string _token;
};
