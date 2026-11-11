// 定义传输层调用应用命令的边界，不暴露具体连接实现。
#pragma once

#include <functional>
#include <string>

// 与传输层无关的命令接口。传输层提供连接标识并报告断开；应用命令决定
// 是否将该标识用作显式启用的临时控制来源。
// 只有传输层借用此服务；命令、断开清理和服务析构都不能反向借用传输接口。
class ICommandService {
public:
    virtual ~ICommandService() = default;
    // 执行 JSON 命令并返回序列化响应；connection 是传输层提供的连接标识。
    virtual std::string Execute(const std::string& json, const std::string& connection) = 0;
    // 完成回调在传输层所属线程最多执行一次。回调不持有传输对象，
    // 其生命周期可以超过已断开的连接。
    virtual void ExecuteAsync(const std::string& json, const std::string& connection, std::function<void(std::string)> complete) { complete(Execute(json, connection)); }
    // 连接断开时撤销与 connection 关联的临时控制状态。
    virtual void Disconnected(const std::string& connection) = 0;
};
