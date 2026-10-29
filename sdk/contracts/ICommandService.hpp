#pragma once

#include <string>
#include <functional>

// 与传输层无关的命令接口。传输层提供连接标识并报告断开；应用命令决定
// 是否将该标识用作显式启用的临时控制来源。
// 只有传输层借用此服务；命令、断开清理和服务析构都不能反向借用传输接口。
class ICommandService
{
public:
    virtual ~ICommandService() = default;
    virtual std::string Execute(const std::string& json, const std::string& connection) = 0;
    // 完成回调在传输层所属线程最多执行一次。回调不持有传输对象，
    // 其生命周期可以超过已断开的连接。
    virtual void ExecuteAsync(const std::string& json, const std::string& connection,
        std::function<void(std::string)> complete) { complete(Execute(json, connection)); }
    virtual void Disconnected(const std::string& connection) = 0;
};
