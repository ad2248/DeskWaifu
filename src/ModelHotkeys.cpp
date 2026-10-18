#include "ModelHotkeys.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <Utils/CubismJson.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismModel.hpp>

using namespace Live2D::Cubism::Framework;
namespace
{
using Json = std::unique_ptr<Utils::CubismJson, decltype(&Utils::CubismJson::Delete)>;
Json ReadJson(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    // Cubism's numeric parser only accepts a comma or newline after a number.
    // Normalize insignificant whitespace/delimiters so compact JSON works too.
    std::string normalized;
    bool quoted = false, escaped = false;
    for (char c : data)
    {
        if (!quoted && (c == '}' || c == ']')) normalized += '\n';
        normalized += !quoted && std::isspace(static_cast<unsigned char>(c)) ? '\n' : c;
        if (escaped) escaped = false;
        else if (quoted && c == '\\') escaped = true;
        else if (c == '"') quoted = !quoted;
    }
    return Json(normalized.empty() ? nullptr : Utils::CubismJson::Create(
        reinterpret_cast<const csmByte*>(normalized.data()), normalized.size()), Utils::CubismJson::Delete);
}
std::string Text(Utils::Value& value)
{
    return value.IsString() ? value.GetRawString() : "";
}
bool RelativePath(const std::string& path)
{
    return !path.empty() && path.front() != '/' && path.find("..") == std::string::npos
        && path.find('\\') == std::string::npos;
}
float Nonnegative(Utils::Value& value, float fallback)
{
    const float number = value.ToFloat(fallback);
    return std::isfinite(number) ? std::max(0.0f, number) : fallback;
}
}

std::vector<std::string> ModelHotkeys::ExpressionDirectories()
{
    return {"", "Expressions/", "exp/"};
}

void ModelHotkeys::Load(const std::string& directory, const std::string& modelFile)
{
    _bindings.clear();
    _expressions.clear();
    _error.clear();
    Reset();
    const std::string suffix = ".model3.json";
    if (modelFile.size() < suffix.size() || modelFile.compare(modelFile.size() - suffix.size(), suffix.size(), suffix) != 0) return;
    const std::string path = directory + modelFile.substr(0, modelFile.size() - suffix.size()) + ".vtube.json";
    if (!std::ifstream(path).good()) return;
    auto json = ReadJson(path);
    if (!json || !json->GetRoot()["Hotkeys"].IsArray())
    {
        _error = "Invalid VTube Studio hotkey configuration";
        return;
    }
    auto& hotkeys = json->GetRoot()["Hotkeys"];
    std::set<std::string> ids;
    for (int i = 0; i < hotkeys.GetSize(); ++i)
    {
        auto& source = hotkeys[i];
        Binding binding;
        binding.id = Text(source["HotkeyID"]);
        binding.file = Text(source["File"]);
        binding.name = Text(source["Name"]);
        if (binding.name.empty()) binding.name = binding.file;
        binding.action = Text(source["Action"]);
        binding.enabled = source["IsActive"].ToBoolean(true);
        binding.fade = Nonnegative(source["FadeSecondsAmount"], 0.5f);
        if (source["DeactivateAfterSeconds"].ToBoolean())
            binding.duration = Nonnegative(source["DeactivateAfterSecondsAmount"], 10);
        for (const char* key : {"Trigger1", "Trigger2", "Trigger3"})
        {
            const auto text = Text(source["Triggers"][key]);
            if (!text.empty()) binding.keys.push_back(text);
        }
        if (binding.id.empty() || !ids.insert(binding.id).second)
            binding.reason = "Missing or duplicate hotkey ID";
        else if (binding.action != "ToggleExpression")
            binding.reason = "Unsupported action";
        else if (source["DeactivateAfterKeyUp"].ToBoolean())
            binding.reason = "Release-to-deactivate is not supported";
        else
        {
            std::vector<std::string> candidates;
            const auto folder = Text(source["Folder"]);
            if (!folder.empty() && RelativePath(folder)) candidates.push_back(folder + "/" + binding.file);
            for (const auto& prefix : ExpressionDirectories()) candidates.push_back(prefix + binding.file);
            for (const auto& candidate : candidates)
            {
                if (!RelativePath(candidate)) continue;
                auto expressionJson = ReadJson(directory + candidate);
                if (!expressionJson || !expressionJson->GetRoot()["Parameters"].IsArray()) continue;
                int index = 0;
                while (index < static_cast<int>(_expressions.size()) && _expressions[index].file != candidate) ++index;
                if (index == static_cast<int>(_expressions.size()))
                {
                    Expression expression;
                    expression.file = candidate;
                    auto& parameters = expressionJson->GetRoot()["Parameters"];
                    for (int p = 0; p < parameters.GetSize(); ++p)
                    {
                        auto& parameter = parameters[p];
                        auto blend = Text(parameter["Blend"]);
                        if (blend.empty()) blend = "Add";
                        if (!Text(parameter["Id"]).empty() && parameter["Value"].IsFloat()
                            && (blend == "Add" || blend == "Multiply" || blend == "Overwrite"))
                            expression.parameters.push_back({Text(parameter["Id"]), blend, parameter["Value"].ToFloat()});
                    }
                    _expressions.push_back(expression);
                }
                if (!_expressions[index].parameters.empty()) binding.expression = index;
                break;
            }
            if (binding.expression < 0) binding.reason = "Expression file missing or invalid";
        }
        _bindings.push_back(binding);
    }
}

bool ModelHotkeys::IsActive(const std::string& id) const
{
    for (const auto& binding : _bindings)
        if (binding.id == id)
            return binding.expression >= 0 && _expressions[binding.expression].active;
    return false;
}

void ModelHotkeys::Reset()
{
    static unsigned long generation = 0;
    _token = "model-" + std::to_string(++generation);
    for (auto& expression : _expressions)
    {
        expression.active = false;
        expression.weight = 0;
        expression.remaining = -1;
    }
}

bool ModelHotkeys::Trigger(const std::string& id, std::string& error)
{
    for (const auto& binding : _bindings)
    {
        if (binding.id != id) continue;
        if (!binding.enabled || !binding.reason.empty())
        {
            error = binding.enabled ? binding.reason : "Hotkey disabled";
            return false;
        }
        auto& expression = _expressions[binding.expression];
        expression.active = !expression.active;
        expression.fade = binding.fade;
        expression.remaining = expression.active ? binding.duration : -1;
        return true;
    }
    error = "Hotkey not found (the model may have changed)";
    return false;
}

void ModelHotkeys::Update(float seconds)
{
    for (auto& expression : _expressions)
    {
        if (expression.active && expression.remaining >= 0)
        {
            expression.remaining -= seconds;
            if (expression.remaining <= 0) expression.active = false;
        }
        const float step = expression.fade > 0 ? seconds / expression.fade : 1;
        expression.weight = expression.active ? std::min(1.0f, expression.weight + step)
            : std::max(0.0f, expression.weight - step);
    }
}

void ModelHotkeys::Apply(CubismModel* model) const
{
    // The caller restores the saved base before this pass on every frame.
    for (const auto& expression : _expressions)
    {
        if (expression.weight <= 0) continue;
        for (const auto& parameter : expression.parameters)
        {
            const auto id = CubismFramework::GetIdManager()->GetId(parameter.id.c_str());
            if (parameter.blend == "Multiply")
                model->MultiplyParameterValue(id, parameter.value, expression.weight);
            else if (parameter.blend == "Overwrite")
                model->SetParameterValue(id, parameter.value, expression.weight);
            else
                model->AddParameterValue(id, parameter.value, expression.weight);
        }
    }
}
