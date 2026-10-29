#pragma once

#include <string>
#include <map>
#include <deque>

class InteractionContext;

class AppCommandHandler
{
public:
    explicit AppCommandHandler(InteractionContext& app) : _app(app) {}
    std::string Execute(const std::string& json, const std::string& connection);
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
