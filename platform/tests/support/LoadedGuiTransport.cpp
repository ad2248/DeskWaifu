#include "LoadedGuiTransport.hpp"
#include "plugin/GuiPluginManager.hpp"
#include "plugin/GuiTransport.hpp"
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QTabWidget>
#include <QTemporaryDir>
#include <stdexcept>

struct LoadedGuiTransport::Impl
{
    QTemporaryDir installation;
    QTabWidget tabs;
    QPointer<IGuiTransport> transport;
    std::unique_ptr<GuiPluginManager> plugins;
};
LoadedGuiTransport::LoadedGuiTransport() : _impl(new Impl)
{
    const auto root = _impl->installation.path();
    if (!_impl->installation.isValid() || !QDir().mkpath(root + "/ipc") ||
        !QFile::copy(QStringLiteral(IPC_PLUGIN_FILE), root + "/ipc/ipc.so"))
        throw std::runtime_error("install actual IPC transport plugin");
    _impl->plugins.reset(new GuiPluginManager(root, *this, _impl->tabs));
    QString error;
    if (!_impl->plugins->Start(&error) || !_impl->transport)
        throw std::runtime_error(error.isEmpty() ? "IPC plugin did not register transport" : error.toStdString());
}
LoadedGuiTransport::~LoadedGuiTransport() { _impl->plugins->Stop(); }
bool LoadedGuiTransport::isConnected() const { return _impl->transport && _impl->transport->isConnected(); }
void LoadedGuiTransport::connectToServer() { if (_impl->transport) _impl->transport->connectToServer(); }
void LoadedGuiTransport::sendCommand(const QString& command, const QJsonObject& parameters)
{ if (_impl->transport) _impl->transport->sendCommand(command, parameters); }
void LoadedGuiTransport::cancelQueuedCommand(const QString& command)
{ if (_impl->transport) _impl->transport->cancelQueuedCommand(command); }
bool LoadedGuiTransport::registerTransport(IGuiTransport* transport, QObject*)
{
    if (!transport || _impl->transport) return false;
    _impl->transport = transport;
    connect(transport, &GuiHostServices::connectionChanged, this, &GuiHostServices::connectionChanged);
    connect(transport, &GuiHostServices::responseReceived, this, &GuiHostServices::responseReceived);
    connect(transport, &GuiHostServices::protocolError, this, &GuiHostServices::protocolError);
    connect(transport, &GuiHostServices::commandOutcomeUnknown, this, &GuiHostServices::commandOutcomeUnknown);
    connect(transport, &GuiHostServices::commandCancelled, this, &GuiHostServices::commandCancelled);
    return true;
}
void LoadedGuiTransport::unregisterTransport(IGuiTransport* transport)
{
    if (_impl->transport != transport) return;
    transport->disconnect(this); _impl->transport.clear();
}
QString LoadedGuiTransport::socketPath() const
{ return qEnvironmentVariable("XDG_RUNTIME_DIR") + "/deskwaifu.sock"; }
