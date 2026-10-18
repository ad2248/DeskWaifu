#pragma once

#include <string>
#include <vector>

namespace Live2D { namespace Cubism { namespace Framework { class CubismModel; } } }

// Independent of the renderer: model configuration and persistent expression state.
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
    struct Binding
    {
        std::string id, name, action, file, reason;
        std::vector<std::string> keys;
        bool enabled = true;
        float fade = 0.5f, duration = -1;
        int expression = -1;
    };

    void Load(const std::string& directory, const std::string& modelFile);
    void Reset();
    bool Trigger(const std::string& id, std::string& error);
    void Update(float seconds);
    void Apply(Live2D::Cubism::Framework::CubismModel* model) const;
    // Resolve IDs against this model's current bindings, never a caller's index.
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
