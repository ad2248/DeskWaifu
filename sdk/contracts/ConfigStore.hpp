#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// 存储支持行注释的 JSON 文档，并以整个文档为单位原子写入。
// 只有此类负责写入文档；业务校验由调用方完成。
class ConfigStore
{
public:
    struct Snapshot
    {
        std::string document;
        std::map<std::string, std::string> values;
    };

    // 私有配置在每次写入时强制 0600，默认仍保留已有文件权限。
    explicit ConfigStore(std::string path, bool privatePermissions = false)
        : _path(std::move(path)), _privatePermissions(privatePermissions) {}
    const std::string& Path() const { return _path; }
    bool Read(Snapshot& snapshot, std::string& error) const;
    bool Commit(const std::string& document, const Snapshot& expected, Snapshot& saved,
        std::string& error, bool overwrite = false) const;
    static bool Parse(const std::string& document, Snapshot& snapshot, std::string& error, bool allowComments = true);
    static bool ParseString(const std::string& json, std::string& value);
    static bool ParseStringArray(const std::string& json, std::vector<std::string>& values);

private:
    std::string _path;
    bool _privatePermissions;
};
