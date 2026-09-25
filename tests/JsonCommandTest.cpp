#include "JsonHelpers.hpp"

#include <string>

int main()
{
    for (const std::string key : {"stream_source", "reset"}) {
        if (!JsonGetBool("{\"" + key + "\":true}", key) ||
            !JsonGetBool("{\"" + key + "\":\"true\"}", key) ||
            !JsonGetBool("{\"" + key + "\":\"1\"}", key) ||
            JsonGetBool("{\"" + key + "\":false}", key) ||
            JsonGetBool("{\"" + key + "\":\"false\"}", key) ||
            JsonGetBool("{\"" + key + "\":\"0\"}", key) ||
            JsonGetBool("{}", key)) return 1;
    }

    return 0;
}
