#include "LAppConfig.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <cstdio>
#include <regex>

namespace {
// Advance past a complete quoted token so braces, colons and escaped quotes
// inside string values never participate in the structural scan.
void SkipJsonString(const std::string& content, size_t& pos)
{
    ++pos;
    while (pos < content.size())
    {
        const char c = content[pos++];
        if (c == '"') return;
        if (c == '\\' && pos < content.size()) ++pos;
    }
}

bool FindTopLevelModelRegion(const std::string& content, std::string& value)
{
    const size_t root = content.find_first_not_of(" \t\r\n");
    if (root == std::string::npos || content[root] != '{') return false;
    int depth = 0;
    for (size_t pos = root; pos < content.size();)
    {
        const char c = content[pos];
        if (c == '"')
        {
            const size_t start = pos;
            SkipJsonString(content, pos);
            if (depth != 1 || content.substr(start, pos - start) != "\"model_region\"") continue;
            const size_t colon = content.find_first_not_of(" \t\r\n", pos);
            // The same token as a string value or array element is not a key.
            if (colon == std::string::npos || content[colon] != ':') continue;
            const size_t begin = colon + 1;
            pos = begin;
            int nested = 0;
            while (pos < content.size())
            {
                const char token = content[pos];
                if (token == '"') { SkipJsonString(content, pos); continue; }
                if (nested == 0 && (token == ',' || token == '}')) break;
                if (token == '{' || token == '[') ++nested;
                else if (token == '}' || token == ']') --nested;
                ++pos;
            }
            value = content.substr(begin, pos - begin);
            return true;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']')
        {
            --depth;
            if (depth == 0) break;
        }
        ++pos;
    }
    return false;
}

bool ParseModelRegion(const std::string& content, bool& configured, ModelRegion& region)
{
    std::string value;
    if (!FindTopLevelModelRegion(content, value)) return true;
    const std::regex object("\\s*\\{([^{}]*)\\}\\s*");
    std::smatch match;
    bool valid = std::regex_match(value, match, object);
    if (valid)
    {
        std::string members = match[1].str();
        const std::regex member("^\\s*\"(x|y|width|height)\"\\s*:\\s*(-?(?:0|[1-9][0-9]*))\\s*(,|$)");
        bool seen[4] = {};
        while (!members.empty())
        {
            if (!std::regex_search(members, match, member)) { valid = false; break; }
            const std::string key = match[1].str();
            const int index = key == "x" ? 0 : key == "y" ? 1 : key == "width" ? 2 : 3;
            if (seen[index]) { valid = false; break; }
            seen[index] = true;
            int value;
            try { value = std::stoi(match[2].str()); }
            catch (...) { valid = false; break; }
            if (index == 0) region.x = value;
            else if (index == 1) region.y = value;
            else if (index == 2) region.width = value;
            else region.height = value;
            const bool comma = match[3].str() == ",";
            members = match.suffix().str();
            if (comma && members.find_first_not_of(" \t\r\n") == std::string::npos)
            { valid = false; break; }
        }
        valid = valid && seen[2] && seen[3] && region.width > 0 && region.height > 0;
    }
    if (!valid)
    {
        std::fprintf(stderr, "[Config] Invalid model_region: expected integer x/y and positive integer width/height\n");
        return false;
    }
    configured = true;
    return true;
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

std::string LAppConfig::Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string LAppConfig::ParseStringValue(const std::string& s, const std::string& key)
{
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return "";

    pos = s.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";

    pos = s.find('"', pos + 1);
    if (pos == std::string::npos) return "";

    size_t end = s.find('"', pos + 1);
    if (end == std::string::npos) return "";

    return s.substr(pos + 1, end - pos - 1);
}

float LAppConfig::ParseFloatValue(const std::string& s, const std::string& key, float defaultVal)
{
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return defaultVal;

    pos = s.find(':', pos + pattern.size());
    if (pos == std::string::npos) return defaultVal;

    pos++;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;

    try {
        return std::stof(s.substr(pos));
    } catch (...) {
        return defaultVal;
    }
}

int LAppConfig::ParseIntValue(const std::string& s, const std::string& key, int defaultVal)
{
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return defaultVal;

    pos = s.find(':', pos + pattern.size());
    if (pos == std::string::npos) return defaultVal;

    pos++;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;

    try {
        return std::stoi(s.substr(pos));
    } catch (...) {
        return defaultVal;
    }
}

std::vector<std::string> LAppConfig::ParseStringArray(const std::string& s, const std::string& key)
{
    std::vector<std::string> result;
    std::string pattern = "\"" + key + "\"";
    size_t pos = s.find(pattern);
    if (pos == std::string::npos) return result;

    pos = s.find('[', pos + pattern.size());
    if (pos == std::string::npos) return result;

    size_t end = s.find(']', pos);
    if (end == std::string::npos) return result;

    std::string arrayContent = s.substr(pos + 1, end - pos - 1);

    size_t cur = 0;
    while (cur < arrayContent.size())
    {
        size_t qStart = arrayContent.find('"', cur);
        if (qStart == std::string::npos) break;

        size_t qEnd = arrayContent.find('"', qStart + 1);
        if (qEnd == std::string::npos) break;

        result.push_back(arrayContent.substr(qStart + 1, qEnd - qStart - 1));
        cur = qEnd + 1;
    }

    return result;
}

std::string LAppConfig::StripComments(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    bool inString = false;

    for (size_t i = 0; i < s.size(); i++)
    {
        if (inString)
        {
            result += s[i];
            if (s[i] == '\\' && i + 1 < s.size())
            {
                result += s[++i];
            }
            else if (s[i] == '"')
            {
                inString = false;
            }
        }
        else if (s[i] == '"')
        {
            inString = true;
            result += s[i];
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            // Skip until end of line
            while (i < s.size() && s[i] != '\n') i++;
            if (i < s.size()) result += '\n';
        }
        else
        {
            result += s[i];
        }
    }

    return result;
}

bool LAppConfig::LoadFromFile(const std::string& path)
{
    sourcePath = path;
    std::ifstream file(path);
    if (!file.is_open())
    {
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = StripComments(ss.str());

    bool regionConfigured = false;
    ModelRegion region;
    if (!ParseModelRegion(content, regionConfigured, region)) return false;

    additionalModelDirs = ParseStringArray(content, "additional_model_dirs");
    for (size_t i = 0; i < additionalModelDirs.size(); i++)
    {
        additionalModelDirs[i] = ExpandTilde(additionalModelDirs[i]);
    }
    defaultModel = ParseStringValue(content, "default_model");
    emotionTimeout = ParseFloatValue(content, "emotion_timeout", 5.0f);
    modelScale = ParseFloatValue(content, "model_scale", 1.0f);
    modelX = ParseFloatValue(content, "model_x", 0.0f);
    modelY = ParseFloatValue(content, "model_y", 0.0f);
    windowWidth = ParseIntValue(content, "window_width", 1900);
    windowHeight = ParseIntValue(content, "window_height", 1000);
    hasModelRegion = regionConfigured;
    modelRegion = region;

    return true;
}
