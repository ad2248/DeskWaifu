#pragma once

#include <string>
#include <map>
#include <deque>

class InteractionContext;

// 在 GUI 交互域解析应用命令，按连接管理编辑事务及有界保存回执。
// 借用 app，调用方须保证上下文比命令处理器存活更久。
class AppCommandHandler
{
public:
    explicit AppCommandHandler(InteractionContext& app) : _app(app) {}
    // 执行 JSON 请求并返回 JSON 成功结果或错误；connection 标识事务所有者。
    std::string Execute(const std::string& json, const std::string& connection);
    // 连接断开时取消其未提交编辑，并释放该连接的外部控制来源。
    void Disconnected(const std::string& connection);

private:
    InteractionContext& _app;
    std::map<std::string, std::string> _saveReceipts;
    std::deque<std::string> _receiptOrder;
    void RememberSave(const std::string& id, const std::string& result)
    {
        if (_saveReceipts.count(id)) { _saveReceipts[id] = result; return; }
        if (_saveReceipts.size() >= 64) { _saveReceipts.erase(_receiptOrder.front()); _receiptOrder.pop_front(); }
        _receiptOrder.push_back(id);
        _saveReceipts[id] = result;
    }
};
