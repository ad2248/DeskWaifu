#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// JSON-with-line-comments document storage with atomic whole-document writes.
// Only this class writes the document; business validation belongs to callers.
class ConfigStore
{
public:
    struct Snapshot
    {
        std::string document;
        std::map<std::string, std::string> values;
    };

    explicit ConfigStore(std::string path) : _path(std::move(path)) {}
    const std::string& Path() const { return _path; }
    bool Read(Snapshot& snapshot, std::string& error) const;
    bool Commit(const std::string& document, const Snapshot& expected, Snapshot& saved,
        std::string& error, bool overwrite = false) const;
    static bool Parse(const std::string& document, Snapshot& snapshot, std::string& error, bool allowComments = true);
    static bool ParseString(const std::string& json, std::string& value);
    static bool ParseStringArray(const std::string& json, std::vector<std::string>& values);

private:
    std::string _path;
};
