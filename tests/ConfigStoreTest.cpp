#include "ConfigStore.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main()
{
    std::string error;
    ConfigStore::Snapshot parsed;
    CHECK(ConfigStore::Parse("{ // comment\n\"url\":\"https://example//x\",\"nested\":{\"x\":1},\"arr\":[true,null,1e-2]}", parsed, error));
    for (auto text : {"{", "{\"a\":1,}", "{\"a\":01}", "{\"a\":1,\"a\":2}",
             "{\"a\":1,\"\\u0061\":2}", "{\"nested\":{\"a\":1,\"a\":2}}", "{\"a\":NaN}", "{\"a\":true} trailing"})
        CHECK(!ConfigStore::Parse(text, parsed, error));
    CHECK(ConfigStore::Parse("{\"模型\":\"\\ud83d\\ude00\"}", parsed, error));
    CHECK(!ConfigStore::Parse("{\"a\":\"\xc0\xaf\"}", parsed, error));
    CHECK(!ConfigStore::Parse("{\"a\":\"\\ud800\"}", parsed, error));
    char directory[] = "/tmp/opencode/config-store-test-XXXXXX";
    CHECK(mkdtemp(directory));
    const std::string path = std::string(directory) + "/config.json";
    auto write = [&](const std::string& text) { std::ofstream file(path); file << text; };
    write("{\n // keep this comment\n \"other\": {\"model_x\": 9},\n \"model_x\": 1, // keep this too\n \"model_region\": {\"x\":100,\"width\":800,\"height\":600}\n}\n");
    CHECK(chmod(path.c_str(), 0640) == 0);
    ConfigStore store(path); ConfigStore::Snapshot before, saved;
    CHECK(store.Read(before, error));
    CHECK(store.Commit({{"model_x", "2.5"}, {"model_y", "-1"}}, before, saved, error));
    CHECK(saved.values.at("model_x") == "2.5");
    CHECK(saved.values.at("other") == "{\"model_x\":9}");
    CHECK(saved.document.find("// keep this comment") != std::string::npos);
    CHECK(saved.document.find("// keep this too") != std::string::npos);
    struct stat st; CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0640);
    CHECK(!store.Commit({{"model_x", "3"}}, before, parsed, error));
    // Unrelated external edits merge without losing them.
    auto expected = saved;
    CHECK(store.Commit({{"other", "[1,2,3]"}}, saved, parsed, error));
    CHECK(store.Commit({{"model_x", "4"}}, expected, saved, error));
    CHECK(saved.values.at("other") == "[1,2,3]");
    CHECK(store.Commit({{"model_x", ""}, {"model_y", ""}, {"model_region", ""}}, saved, parsed, error));
    CHECK(parsed.values.size() == 1 && parsed.values.count("other"));
    CHECK(parsed.document.find("// keep this too") != std::string::npos);
    CHECK(store.Commit({{"other", ""}}, parsed, saved, error));
    CHECK(saved.values.empty());
    CHECK(store.Commit({{"model_x", "5"}}, saved, parsed, error));
    CHECK(store.Commit({{"model_x", "6"}}, saved, saved, error, true));
    CHECK(saved.values.at("model_x") == "6");
    write("{broken");
    CHECK(!store.Commit({{"model_x", "0"}}, saved, parsed, error, true));
    std::ifstream invalid(path); std::string actual; std::getline(invalid, actual); CHECK(actual == "{broken");
    write("{}");
    std::string link = std::string(directory) + "/link.json";
    CHECK(symlink(path.c_str(), link.c_str()) == 0);
    ConfigStore linked(link); CHECK(linked.Read(before, error));
    CHECK(linked.Commit({{"model_x", "7"}}, before, saved, error));
    CHECK(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(store.Read(parsed, error) && parsed.values.at("model_x") == "7");
    CHECK(unlink(link.c_str()) == 0);
    CHECK(symlink("missing.json", link.c_str()) == 0);
    CHECK(!linked.Read(parsed, error));
    CHECK(!linked.Commit({{"model_x", "0"}}, before, saved, error));
    CHECK(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(unlink(link.c_str()) == 0);
    CHECK(unlink(path.c_str()) == 0);
    ConfigStore missing(std::string(directory) + "/new/config.json");
    CHECK(missing.Read(before, error));
    CHECK(missing.Commit({{"model_scale", "1"}}, before, saved, error));
    CHECK(unlink(missing.Path().c_str()) == 0);
    CHECK(rmdir((std::string(directory) + "/new").c_str()) == 0);
    CHECK(rmdir(directory) == 0);
    return 0;
}
