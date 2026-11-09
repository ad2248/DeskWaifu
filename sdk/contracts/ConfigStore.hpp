// 提供带行注释 JSON 配置的读取、校验、冲突检测及原子保存。
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
    // 保留原始文档及去除无关空白、行注释后的顶层 JSON 值，供冲突检查使用。
    struct Snapshot
    {
        std::string document;
        std::map<std::string, std::string> values;
    };

    // 私有配置在每次写入时强制 0600，默认仍保留已有文件权限。
    explicit ConfigStore(std::string path, bool privatePermissions = false)
        : _path(std::move(path)), _privatePermissions(privatePermissions) {}
    const std::string& Path() const { return _path; }
    // 读取并校验配置；文件不存在时按空对象处理。成功更新 snapshot 并清空 error，
    // 失败返回 false、填写 error 且保留 snapshot；拒绝非普通文件和超过 4 MiB 的文件。
    bool Read(Snapshot& snapshot, std::string& error) const;
    // 校验并原子替换整个文档，成功时更新 saved 并清空 error。
    // 默认比较当前顶层值与 expected，overwrite 仅跳过此比较，不跳过保存期间的变更检查。
    // 失败返回 false 并填写 error；不提供针对任意外部写入者的原子比较交换保证。
    bool Commit(const std::string& document, const Snapshot& expected, Snapshot& saved,
        std::string& error, bool overwrite = false) const;
    // 解析顶层对象并拒绝重复键，allowComments 控制是否接受 // 行注释。
    // 成功更新 snapshot 并清空 error；失败保留 snapshot，填写 error 并返回 false。
    static bool Parse(const std::string& document, Snapshot& snapshot, std::string& error, bool allowComments = true);
    // 解码单个 JSON 字符串；失败返回 false 且不改变 value。
    static bool ParseString(const std::string& json, std::string& value);
    // 解码仅含字符串的 JSON 数组；失败返回 false，values 可能已被部分更新。
    static bool ParseStringArray(const std::string& json, std::vector<std::string>& values);

private:
    std::string _path;
    bool _privatePermissions;
};
