#include "contracts/ConfigStore.hpp"
#include "TestTemporaryDirectory.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <dirent.h>

namespace { bool failRename = false; }
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to)
{
    if (failRename) { errno = EIO; return -1; }
    return __real_rename(from, to);
}

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
    std::string string;
    std::vector<std::string> strings;
    CHECK(ConfigStore::ParseString("\"\\u5915\\u989c\\ud83d\\ude00\\n\"", string) && string == "夕颜😀\n");
    CHECK(ConfigStore::ParseStringArray("[\"a\", \"b\\\\c\", \"\\\"\"]", strings));
    CHECK(strings.size() == 3 && strings[1] == "b\\c" && strings[2] == "\"");
    CHECK(ConfigStore::ParseStringArray("[]", strings) && strings.empty());
    CHECK(!ConfigStore::ParseString("null", string));
    CHECK(!ConfigStore::ParseString("\"a\" trailing", string));
    CHECK(!ConfigStore::ParseStringArray("[\"a\",1]", strings));
    CHECK(!ConfigStore::ParseStringArray("[\"a\",]", strings));
    const std::string directory = MakeTestTemporaryDirectory("config-store-test");
    CHECK(!directory.empty());
    if (const char* root = std::getenv("TMPDIR"))
        CHECK(directory.find(std::string(root) + "/") == 0);
    const std::string path = std::string(directory) + "/config.json";
    auto write = [&](const std::string& text) { std::ofstream file(path); file << text; };
    write("{\n // keep this comment\n \"other\": {\"model_x\": 9},\n \"model_x\": 1, // keep this too\n \"model_region\": {\"x\":100,\"width\":800,\"height\":600}\n}\n");
    CHECK(chmod(path.c_str(), 0640) == 0);
    ConfigStore store(path); ConfigStore::Snapshot before, saved;
    CHECK(store.Read(before, error));
    CHECK(store.Commit("{\"model_x\":2.5,\"model_y\":-1}", before, saved, error));
    CHECK(saved.values.at("model_x") == "2.5");
    CHECK(saved.values.size() == 2 && saved.values.at("model_y") == "-1");
    CHECK(saved.document.find("// keep this comment") == std::string::npos);
    CHECK(!saved.values.count("other") && !saved.values.count("model_region"));
    struct stat st; CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0640);
    CHECK(!store.Commit("{\"model_x\":3}", before, parsed, error));
    // 整个文档写入也必须检测无关字段的外部编辑。
    auto expected = saved;
    CHECK(store.Commit("{\"other\":[1,2,3]}", saved, parsed, error));
    CHECK(!store.Commit("{\"model_x\":4}", expected, saved, error));
    CHECK(store.Read(saved, error) && saved.values.at("other") == "[1,2,3]");
    CHECK(store.Commit("{}", saved, saved, error));
    CHECK(saved.values.empty());
    CHECK(store.Commit("{\"model_x\":5}", saved, parsed, error));
    CHECK(store.Commit("{\"model_x\":6}", saved, saved, error, true));
    CHECK(saved.values.at("model_x") == "6");
    // A real filesystem commit failure must not report success, change the
    // output snapshot, replace disk content or leak its temporary file.
    auto unchanged = saved;
    const auto document = saved.document;
    failRename = true;
    CHECK(!store.Commit("{\"model_x\":99}", saved, unchanged, error));
    failRename = false;
    CHECK(!error.empty() && unchanged.document == document && unchanged.values == saved.values);
    CHECK(store.Read(parsed, error) && parsed.document == document);
    DIR* entries = opendir(directory.c_str()); CHECK(entries);
    bool leaked = false;
    while (const auto* entry = readdir(entries)) if (std::string(entry->d_name).find(".deskwaifu-config-") == 0) leaked = true;
    closedir(entries); CHECK(!leaked);
    CHECK(store.Commit("{\"model_x\":6}", saved, saved, error)); // retry succeeds after the boundary recovers.
    CHECK(!store.Commit("{broken", saved, parsed, error));
    CHECK(store.Read(parsed, error) && parsed.values == saved.values);
    write("{broken");
    CHECK(!store.Commit("{\"model_x\":0}", saved, parsed, error, true));
    std::ifstream invalid(path); std::string actual; std::getline(invalid, actual); CHECK(actual == "{broken");
    write("{}");
    std::string link = std::string(directory) + "/link.json";
    CHECK(symlink(path.c_str(), link.c_str()) == 0);
    ConfigStore linked(link); CHECK(linked.Read(before, error));
    CHECK(linked.Commit("{\"model_x\":7}", before, saved, error));
    CHECK(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(store.Read(parsed, error) && parsed.values.at("model_x") == "7");
    CHECK(unlink(link.c_str()) == 0);
    CHECK(symlink("missing.json", link.c_str()) == 0);
    CHECK(!linked.Read(parsed, error));
    CHECK(!linked.Commit("{\"model_x\":0}", before, saved, error));
    CHECK(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(unlink(link.c_str()) == 0);
    CHECK(unlink(path.c_str()) == 0);
    ConfigStore missing(std::string(directory) + "/new/config.json");
    CHECK(missing.Read(before, error));
    CHECK(missing.Commit("{\"model_scale\":1}", before, saved, error));
    CHECK(unlink(missing.Path().c_str()) == 0);
    CHECK(rmdir((std::string(directory) + "/new").c_str()) == 0);
    CHECK(rmdir(directory.c_str()) == 0);
    return 0;
}
