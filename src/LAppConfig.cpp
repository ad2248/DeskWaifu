#include "LAppConfig.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <pwd.h>
#include <sstream>
#include <unistd.h>

namespace {
std::string Quote(const std::string& text)
{
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 0x20) {
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += c;
    }
    return result + '"';
}
}

static LAppConfig s_config;

LAppConfig& LAppConfig::GetInstance()
{
    return s_config;
}

std::string LAppConfig::GetDefaultConfigPath()
{
    const char* configHome = getenv("XDG_CONFIG_HOME");
    if (configHome && strlen(configHome) > 0)
    {
        return std::string(configHome) + "/deskwaifu/config.json";
    }
    const char* homeDir = getenv("HOME");
    if (!homeDir || strlen(homeDir) == 0)
    {
        homeDir = getpwuid(getuid())->pw_dir;
    }
    return std::string(homeDir) + "/.config/deskwaifu/config.json";
}

std::string LAppConfig::ExpandTilde(const std::string& path)
{
    if (path.empty() || path[0] != '~') return path;

    const char* homeDir = getenv("HOME");
    if (!homeDir || strlen(homeDir) == 0)
    {
        homeDir = getpwuid(getuid())->pw_dir;
    }
    return std::string(homeDir) + path.substr(1);
}

bool LAppConfig::LoadFromFile(const std::string& path)
{
    sourcePath = path;
    // Unlike ConfigStore::Read, startup distinguishes a missing file from {}.
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::stringstream text; text << file.rdbuf();
    ConfigStore::Snapshot snapshot;
    std::string error;
    return ConfigStore::Parse(text.str(), snapshot, error) && Decode(snapshot, values, error);
}

bool LAppConfig::Decode(const ConfigStore::Snapshot& snapshot, Values& values, std::string& error)
{
    Values next;
    const auto string = [&](const char* key, std::string& value) {
        const auto it = snapshot.values.find(key);
        return it == snapshot.values.end() || ConfigStore::ParseString(it->second, value);
    };
    auto dirs = snapshot.values.find("additional_model_dirs");
    if ((dirs != snapshot.values.end() && !ConfigStore::ParseStringArray(dirs->second, next.additionalModelDirs)) ||
        !string("default_model", next.defaultModel)) {
        error = "Invalid model paths in config"; return false;
    }
    if (!string("model_output", next.modelOutput)) {
        error = "Invalid model_output: expected a display name string"; return false;
    }
    // Missing or unconvertible legacy numeric fields keep construction defaults.
    const auto number = [&](const char* key, auto& destination) {
        const auto it = snapshot.values.find(key);
        if (it == snapshot.values.end()) return;
        std::istringstream input(it->second); input.imbue(std::locale::classic());
        auto parsed = destination;
        if ((input >> parsed) && input.peek() == std::char_traits<char>::eof() && std::isfinite(parsed)) destination = parsed;
    };
    number("emotion_timeout", next.emotionTimeout);
    number("model_scale", next.modelScale);
    number("model_x", next.modelX);
    number("model_y", next.modelY);
    number("window_width", next.windowWidth);
    number("window_height", next.windowHeight);
    auto enabled = snapshot.values.find("hover_opacity_enabled");
    if (enabled != snapshot.values.end()) {
        if (enabled->second != "true" && enabled->second != "false") {
            error = "Invalid hover_opacity_enabled: expected a boolean"; return false;
        }
        next.hoverOpacityEnabled = enabled->second == "true";
    }
    for (const auto key : {"hover_opacity", "idle_opacity"}) {
        auto opacity = snapshot.values.find(key);
        if (opacity != snapshot.values.end()) {
            std::istringstream input(opacity->second); input.imbue(std::locale::classic());
            double parsed;
            if (!(input >> parsed) || input.peek() != std::char_traits<char>::eof() ||
                !std::isfinite(parsed) || parsed < 0 || parsed > 1) {
                error = std::string("Invalid ") + key + ": expected a finite number within 0..1"; return false;
            }
            (std::string(key) == "hover_opacity" ? next.hoverOpacity : next.idleOpacity) = static_cast<float>(parsed);
        }
    }
    auto region = snapshot.values.find("model_region");
    if (region != snapshot.values.end()) {
        ConfigStore::Snapshot rectangle;
        if (!ConfigStore::Parse(region->second, rectangle, error)) return false;
        ModelRegion parsed;
        for (const auto& field : rectangle.values) {
            int* destination = field.first == "x" ? &parsed.x : field.first == "y" ? &parsed.y
                : field.first == "width" ? &parsed.width : field.first == "height" ? &parsed.height : nullptr;
            if (!destination || field.second.find_first_not_of("-0123456789") != std::string::npos) {
                error = "Invalid integer model_region"; return false;
            }
            try { *destination = std::stoi(field.second); }
            catch (...) { error = "Invalid integer model_region"; return false; }
        }
        if (!rectangle.values.count("width") || !rectangle.values.count("height") || parsed.width <= 0 || parsed.height <= 0) {
            error = "Crop dimensions must be positive integers"; return false;
        }
        next.modelRegion = parsed;
    }
    values = std::move(next); error.clear(); return true;
}

bool LAppConfig::Serialize(const Values& values, std::string& document, std::string& error)
{
    std::ostringstream out; out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\n  \"additional_model_dirs\": [";
    for (size_t i = 0; i < values.additionalModelDirs.size(); ++i) {
        if (i) out << ", ";
        out << Quote(values.additionalModelDirs[i]);
    }
    out << "],\n  \"default_model\": " << Quote(values.defaultModel)
        << ",\n  \"emotion_timeout\": " << values.emotionTimeout
        << ",\n  \"model_scale\": " << values.modelScale
        << ",\n  \"model_x\": " << values.modelX
        << ",\n  \"model_y\": " << values.modelY
        << ",\n  \"window_width\": " << values.windowWidth
        << ",\n  \"window_height\": " << values.windowHeight
        << ",\n  \"hover_opacity_enabled\": " << (values.hoverOpacityEnabled ? "true" : "false")
        << ",\n  \"hover_opacity\": " << values.hoverOpacity
        << ",\n  \"idle_opacity\": " << values.idleOpacity;
    if (!values.modelOutput.empty()) out << ",\n  \"model_output\": " << Quote(values.modelOutput);
    if (values.modelRegion) {
        const auto& r = *values.modelRegion;
        out << ",\n  \"model_region\": {\"x\": " << r.x << ", \"y\": " << r.y
            << ", \"width\": " << r.width << ", \"height\": " << r.height << '}';
    }
    out << "\n}\n";
    ConfigStore::Snapshot snapshot; Values check;
    const std::string result = out.str();
    if (!ConfigStore::Parse(result, snapshot, error) || !Decode(snapshot, check, error)) return false;
    document = result; return true;
}
