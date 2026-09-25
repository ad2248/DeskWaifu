#pragma once

#include <string>

// Extract a string or unquoted scalar value from a flat JSON object.
inline std::string JsonGetString(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return "";

    if (json[pos] == '"')
    {
        // String value
        pos++;
        std::string val;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                pos++;
            }
            val += json[pos];
            pos++;
        }
        return val;
    }

    // Number or other literal
    std::string val;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ')
    {
        val += json[pos];
        pos++;
    }
    return val;
}
