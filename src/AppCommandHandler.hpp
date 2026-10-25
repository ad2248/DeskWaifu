#pragma once

#include "ICommandService.hpp"
#include <string>
#include <map>
#include <deque>

class LAppDelegate;

class AppCommandHandler : public ICommandService
{
public:
    explicit AppCommandHandler(LAppDelegate& app) : _app(app) {}
    std::string Execute(const std::string& json, const std::string& connection) override;
    void ExecuteAsync(const std::string& json, const std::string& connection,
        std::function<void(std::string)> complete) override;
    void Disconnected(const std::string& connection) override;

private:
    LAppDelegate& _app;
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
