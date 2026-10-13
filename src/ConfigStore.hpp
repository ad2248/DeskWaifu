#pragma once

#include <map>
#include <string>
#include <utility>

// JSON-with-line-comments document storage. Empty patch values delete keys.
// Only this class writes the document; business validation belongs to callers.
class ConfigStore
{
public:
    struct Snapshot
    {
        std::string document;
        std::map<std::string, std::string> values;
    };
    using Patch = std::map<std::string, std::string>;

    explicit ConfigStore(std::string path) : _path(std::move(path)) {}
    const std::string& Path() const { return _path; }
    bool Read(Snapshot& snapshot, std::string& error) const;
    bool Commit(const Patch& patch, const Snapshot& expected, Snapshot& saved,
        std::string& error, bool overwrite = false) const;
    static bool Parse(const std::string& document, Snapshot& snapshot, std::string& error);

private:
    std::string _path;
};
