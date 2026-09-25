#include "JsonHelpers.hpp"

#include <string>

int main()
{
    const auto parsesAsEnabled = [](const std::string& json, const std::string& key)
    {
        const auto value = JsonGetString(json, key);
        return value == "true" || value == "1";
    };

    if (!parsesAsEnabled(R"({"stream_source":true})", "stream_source") ||
        !parsesAsEnabled(R"({"stream_source":"true"})", "stream_source") ||
        !parsesAsEnabled(R"({"stream_source":"1"})", "stream_source") ||
        !parsesAsEnabled(R"({"reset":true})", "reset") ||
        !parsesAsEnabled(R"({"reset":"true"})", "reset") ||
        !parsesAsEnabled(R"({"reset":"1"})", "reset")) return 1;

    return 0;
}
