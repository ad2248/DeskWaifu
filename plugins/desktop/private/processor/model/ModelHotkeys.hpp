#pragma once

#include <string>
#include <vector>
#include "contracts/ModelBinding.hpp"

namespace Live2D { namespace Cubism { namespace Framework { class CubismModel; } } }

// 与渲染器独立的模型配置和持久表情状态。
class ModelHotkeys
{
public:
    struct Parameter { std::string id, blend; float value; };
    struct Expression
    {
        std::string file;
        std::vector<Parameter> parameters;
        bool active = false;
        float weight = 0, fade = 0.5f, remaining = -1;
    };
    using Binding = ModelBinding;

    void Load(const std::string& directory, const std::string& modelFile);
    void Reset();
    bool Trigger(const std::string& id, std::string& error);
    void Update(float seconds);
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
