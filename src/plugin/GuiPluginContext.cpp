#include "GuiPluginContext.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QTabWidget>
#include <QThread>
#include <QVariant>

namespace {
class ScopedServices final : public GuiHostServices
{
public:
    explicit ScopedServices(GuiHostServices& host) : _host(host)
    {
        for (const auto& name : host.dynamicPropertyNames()) setProperty(name.constData(), host.property(name.constData()));
        connect(&host, &GuiHostServices::connectionChanged, this, &GuiHostServices::connectionChanged);
        connect(&host, &GuiHostServices::responseReceived, this, &GuiHostServices::responseReceived);
        connect(&host, &GuiHostServices::protocolError, this, &GuiHostServices::protocolError);
        connect(&host, &GuiHostServices::commandOutcomeUnknown, this, &GuiHostServices::commandOutcomeUnknown);
        connect(&host, &GuiHostServices::commandCancelled, this, &GuiHostServices::commandCancelled);
        connect(&host, &GuiHostServices::logMessage, this, &GuiHostServices::logMessage);
        connect(&host, &GuiHostServices::backendReady, this, &GuiHostServices::backendReady);
        connect(&host, &GuiHostServices::activationChanged, this, &GuiHostServices::activationChanged);
    }
    bool isConnected() const override { AssertThread(); return _valid && _host.isConnected(); }
    bool isActive() const override { AssertThread(); return _valid && _host.isActive(); }
    void sendCommand(const QString& command, const QJsonObject& parameters) override
    { AssertThread(); if (_valid && _active) _host.sendCommand(command, parameters); }
    void cancelQueuedCommand(const QString& command) override
    { AssertThread(); if (_valid && _active) _host.cancelQueuedCommand(command); }
    void setStatusMessage(const QString& message, int timeoutMs) override
    { AssertThread(); if (_valid) _host.setStatusMessage(message, timeoutMs); }
    void connectToServer() override
    { AssertThread(); if (_valid && _active) _host.connectToServer(); }
    void publishLog(const QString& message) override
    { AssertThread(); if (_valid) _host.publishLog(message); }
    void notifyBackendReady() override
    { AssertThread(); if (_valid && _active) _host.notifyBackendReady(); }
    void Activate() { _active = true; }
    void Invalidate()
    {
        _valid = false;
        disconnect(&_host, nullptr, this, nullptr);
        disconnect();
        QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    }
private:
    void AssertThread() const { Q_ASSERT(QThread::currentThread() == thread()); }
    GuiHostServices& _host;
    bool _valid = true, _active = false;
};
}

GuiPluginContext::GuiPluginContext(QString name, GuiHostServices& services, QTabWidget& tabs, QSet<QString>& ids, QSet<QWidget*>& pages)
    : _name(std::move(name)), _services(new ScopedServices(services)), _tabs(tabs), _ids(ids), _registeredPages(pages) {}

GuiPluginContext::~GuiPluginContext() { Invalidate(); DestroyPages(); }

GuiHostServices* GuiPluginContext::Services() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return _services.get();
}

bool GuiPluginContext::RegisterTab(const GuiTabSpec& spec, QWidget* page, QString* error)
{
    if (error) error->clear();
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (QThread::currentThread() != thread()) return fail(QStringLiteral("GUI context must run on the main thread"));
    if (!_valid || _published) return fail(QStringLiteral("Tab registration is closed"));
    if (spec.id.trimmed().isEmpty() || spec.id.contains('/') || spec.title.trimmed().isEmpty())
        return fail(QStringLiteral("Tab local ID and title must not be empty; ID must not contain '/'"));
    const auto id = _name + '/' + spec.id;
    if (_ids.contains(id)) return fail(QStringLiteral("Duplicate tab ID: ") + id);
    if (!page || page->thread() != thread() || page->parent())
        return fail(QStringLiteral("Tab page must be a new, unparented GUI-thread widget"));
    if (_registeredPages.contains(page)) return fail(QStringLiteral("Tab page already registered"));
    _ids.insert(id);
    _registeredPages.insert(page);
    connect(page, &QObject::destroyed, this, [this, page] { _registeredPages.remove(page); });
    _pages.push_back({id, spec.title, spec.order, page});
    return true;
}

void GuiPluginContext::Activate()
{
    static_cast<ScopedServices*>(_services.get())->Activate();
}

void GuiPluginContext::Invalidate()
{
    Q_ASSERT(QThread::currentThread() == thread());
    _valid = false;
    static_cast<ScopedServices*>(_services.get())->Invalidate();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
}

void GuiPluginContext::DestroyPages()
{
    for (auto it = _pages.rbegin(); it != _pages.rend(); ++it) {
        _ids.remove(it->id);
        if (it->page) {
            const int index = _tabs.indexOf(it->page);
            if (index >= 0) _tabs.removeTab(index);
            delete it->page;
        }
    }
    _pages.clear();
}
