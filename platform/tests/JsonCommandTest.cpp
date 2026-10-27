#include "contracts/JsonHelpers.hpp"

#include <iostream>

int main()
{
    ConfigStore::Snapshot request;
    std::string error;
    auto parse = [&](const std::string& json) {
        if (!ConfigStore::Parse(json, request, error, false)) throw std::runtime_error(error);
    };
    try {
        for (const std::string key : {"stream_source", "reset"}) {
            for (const auto* value : {"true", "\"true\"", "1", "\"1\""}) {
                parse("{\"" + key + "\":" + value + "}");
                if (!JsonGetBool(request, key)) return 1;
            }
            for (const auto* value : {"false", "\"false\"", "0", "\"0\""}) {
                parse("{\"" + key + "\":" + value + "}");
                if (JsonGetBool(request, key)) return 1;
            }
        }
        parse("{}");
        if (JsonGetBool(request, "reset") || JsonGetInt(request, "index", -1) != -1 ||
            JsonGetFloat(request, "x", .5f) != .5f) return 1;
        parse(R"({"metadata":{"command":"quit"},"command":"get_status"})");
        if (JsonGetString(request, "command") != "get_status") return 1;
        parse(R"({"note":"command","value":"quit","command":"get_status"})");
        if (JsonGetString(request, "command") != "get_status") return 1;
        parse(R"({"comm\u0061nd":"get_status","id":"\u5915\u989c\n\"\\\ud83d\ude3a"})");
        if (JsonGetString(request, "command") != "get_status" ||
            JsonGetString(request, "id") != "夕颜\n\"\\😺") return 1;
        for (const auto* invalid : {R"({"command":"get_status","command":"quit"})",
                R"({"command":"get_status","comm\u0061nd":"quit"})",
                R"({"command":"get_status"} trailing)", "[]", "{", "{}//comment",
                R"({"command":"\ud800"})", R"({"index":001})"})
            if (ConfigStore::Parse(invalid, request, error, false)) return 1;
        // 配置文档仍支持注释。
        if (!ConfigStore::Parse("{}//comment", request, error)) return 1;
        for (const auto* value : {"0.9", "1e0", "2147483648", "-2147483649", "null", "true", "[]", "{}",
                "\"banana\"", "\"0\"", "\"0junk\""}) {
            parse(std::string("{\"index\":") + value + "}");
            try { JsonGetInt(request, "index", -1); return 1; }
            catch (const std::invalid_argument&) {}
        }
        for (const auto* value : {"\"1\"", "null", "false", "[]", "{}", "1e1000"}) {
            parse(std::string("{\"x\":") + value + "}");
            try { JsonGetFloat(request, "x", 0); return 1; }
            catch (const std::invalid_argument&) {}
        }
        parse(R"({"index":-2147483648,"priority":2147483647,"x":-1.25e-1})");
        if (JsonGetInt(request, "index", 0) != std::numeric_limits<int>::min() ||
            JsonGetInt(request, "priority", 0) != std::numeric_limits<int>::max() ||
            JsonGetFloat(request, "x", 0) != -.125f) return 1;
        parse(R"({"command":true,"reset":null})");
        try { JsonGetString(request, "command"); return 1; } catch (const std::invalid_argument&) {}
        try { JsonGetBool(request, "reset"); return 1; } catch (const std::invalid_argument&) {}
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
