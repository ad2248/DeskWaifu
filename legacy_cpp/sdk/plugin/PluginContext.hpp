// 定义后端插件作用域的端点管理、配置访问和故障报告上下文。
#pragma once

#include "ConfigJson.hpp"
#include "event/ProcessorEventLoop.hpp"

class PluginManager;

// 管理器拥有的后端插件上下文，从 Initialize 到 Stop 及插件析构期间始终存活。
// 端点由上下文拥有，初始化失败也会统一撤销；插件只借用返回的指针。
// 生命周期、端点创建和故障报告须在事件循环所属线程执行。
class PluginContext final {
public:
    const QString& Name() const { return _name; }
    // 返回插件专属配置目录；目录可能尚未创建。
    const QString& ConfigDirectory() const { return _configDirectory; }
    // 返回借用的事件循环，供过渡适配器复用 RAII Processor。
    // 旧式注册仍由适配器负责撤销，不归上下文端点清理机制管理。
    ProcessorEventLoop& Loop() const { return _loop; }
    // 创建全名为 Name()/localName 的端点并保留所有权，激活前仅可登记订阅和能力。
    // 名称为空、含斜杠、重复，处理器无效、上下文失效或线程不符时返回 nullptr。
    EventEndpoint* CreateEndpoint(const QString& localName, EventEndpoint::Handler handler);
    // 在循环所属线程记录首个非空故障，供管理器汇总；失效后的 Stop 阶段仍可调用。
    // 错误消息须由插件脱敏，不得包含凭据或请求正文。
    void ReportFailure(const QString& error);
    // 从专属目录读取 JSON 对象；fileName 必须是单个文件名。
    // 成功返回 true；失败保留 object，并在提供 error 时写入原因。
    bool ReadConfig(const QString& fileName, QJsonObject& object, QString* error = nullptr) const;
    // 在专属目录原子写入 JSON 对象，按需创建目录；fileName 必须是单个文件名。
    // 成功返回 true；失败保留已有目标文件，并在提供 error 时写入原因。
    bool WriteConfig(const QString& fileName, const QJsonObject& object, QString* error = nullptr) const;

private:
    friend class PluginManager;
    PluginContext(ProcessorEventLoop& loop, QString name, QString configDirectory);
    void Activate();
    void Invalidate();
    QString ConfigPath(const QString& fileName, QString* error) const;
    ProcessorEventLoop& _loop;
    QString _name, _configDirectory;
    std::vector<std::unique_ptr<EventEndpoint>> _endpoints;
    bool _active = false, _valid = true;
    QString _failure;
};
