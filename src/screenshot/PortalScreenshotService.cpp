#include "PortalScreenshotService.hpp"
#include <QCoreApplication>
#include <QDBusError>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QPointer>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <utility>

namespace {
const QString Service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString DesktopPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString RequestInterface = QStringLiteral("org.freedesktop.portal.Request");

void CloseRequest(const QDBusConnection& bus, const QString& path)
{
    if (!path.isEmpty()) bus.asyncCall(QDBusMessage::createMethodCall(Service, path, RequestInterface, "Close"));
}

struct DecodedImage { QImage image; QString error; };

DecodedImage ReadImage(const QString& path)
{
    const QFileInfo file(path);
    if (!file.isFile() || file.size() > 64 * 1024 * 1024) return {{}, "screenshot file is missing or exceeds 64 MiB"};
    QImageReader reader(path);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
        qint64(size.width()) * size.height() > 32 * 1024 * 1024 || reader.imageCount() > 1)
        return {{}, "invalid, animated or oversized screenshot"};
    auto image = reader.read();
    if (image.isNull() || image.size() != size) return {{}, "could not decode screenshot: " + reader.errorString()};
    return {image, {}};
}
}

struct PortalScreenshotService::Request
{
    QString id, path;
    bool finished = false, cancelled = false, responded = false, subscribed = false;
};

std::unique_ptr<IScreenshotService> CreateScreenshotService()
{
    return std::unique_ptr<IScreenshotService>(new PortalScreenshotService);
}

PortalScreenshotService::PortalScreenshotService(QObject* parent)
    : PortalScreenshotService(QDBusConnection::sessionBus(), 60000, parent) {}

PortalScreenshotService::PortalScreenshotService(QDBusConnection bus, int timeoutMs, QObject* parent)
    : IScreenshotService(parent), _bus(std::move(bus))
{
    _deadline.setSingleShot(true);
    _deadline.setInterval(timeoutMs);
    connect(&_deadline, &QTimer::timeout, this, [this] {
        if (_request) Fail(_request, Error::TimedOut, "screenshot request timed out");
    });
}

PortalScreenshotService::~PortalScreenshotService()
{
    if (_request) {
        _request->cancelled = _request->finished = true;
        Unsubscribe(_request);
        if (!_request->responded) CloseRequest(_bus, _request->path);
    }
}

bool PortalScreenshotService::Subscribe(const std::shared_ptr<Request>& request, const QString& path)
{
    request->path = path;
    request->subscribed = _bus.connect(Service, path, RequestInterface, "Response", this,
        SLOT(OnResponse(uint,QVariantMap,QDBusMessage)));
    return request->subscribed;
}

void PortalScreenshotService::Unsubscribe(const std::shared_ptr<Request>& request)
{
    if (request->subscribed) {
        _bus.disconnect(Service, request->path, RequestInterface, "Response", this,
            SLOT(OnResponse(uint,QVariantMap,QDBusMessage)));
        request->subscribed = false;
    }
}

QString PortalScreenshotService::Capture(const QString& parentWindow)
{
    if (Busy()) return {};
    const auto request = std::make_shared<Request>();
    request->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    _request = request;
    QString sender = _bus.baseService();
    sender.remove(':'); sender.replace('.', '_');
    const QString token = "deskwaifu_" + QString(request->id).remove('-');
    const QString predicted = DesktopPath + "/request/" + sender + '/' + token;
    if (!_bus.isConnected() || !Subscribe(request, predicted)) {
        QTimer::singleShot(0, this, [this, request] { Fail(request, Error::Unavailable, "screenshot portal bus is unavailable"); });
        return request->id;
    }
    _deadline.start();
    auto call = QDBusMessage::createMethodCall(Service, DesktopPath, "org.freedesktop.portal.Screenshot", "Screenshot");
    call << parentWindow << QVariantMap{{"handle_token", token}, {"interactive", true}, {"modal", true}};
    // Subscribe before the method call: portals can emit Response before returning the handle.
    auto* watcher = new QDBusPendingCallWatcher(_bus.asyncCall(call), QCoreApplication::instance());
    const QPointer<PortalScreenshotService> self(this);
    const auto bus = _bus;
    connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [self, request, bus](QDBusPendingCallWatcher* pending) {
        const QDBusPendingReply<QDBusObjectPath> reply = *pending;
        pending->deleteLater();
        // Cancellation/destruction may precede a delayed method reply. Close its real handle too.
        if (!self || request->cancelled) {
            if (!reply.isError() && !request->responded) CloseRequest(bus, reply.value().path());
            return;
        }
        if (request->finished || request->responded) return;
        if (reply.isError()) {
            const auto code = reply.error().type();
            self->Fail(request, code == QDBusError::ServiceUnknown || code == QDBusError::Disconnected ? Error::Unavailable : Error::BackendFailure,
                reply.error().message());
            return;
        }
        const auto path = reply.value().path();
        if (!path.startsWith(DesktopPath + "/request/") || path.endsWith('/')) {
            self->Fail(request, Error::InvalidResponse, "portal returned an invalid request handle");
            return;
        }
        if (path != request->path) {
            self->Unsubscribe(request);
            if (!self->Subscribe(request, path)) self->Fail(request, Error::Unavailable, "could not subscribe to screenshot response");
        }
    });
    return request->id;
}

bool PortalScreenshotService::Cancel(const QString& requestId)
{
    if (!_request || requestId.isEmpty() || _request->id != requestId) return false;
    Fail(_request, Error::Cancelled, "screenshot request cancelled");
    return true;
}

void PortalScreenshotService::Fail(const std::shared_ptr<Request>& request, Error error, const QString& message)
{
    // Copy before clearing _request: callers may pass that member by reference.
    const auto current = request;
    if (_request != current || current->finished) return;
    current->finished = current->cancelled = true;
    _deadline.stop();
    Unsubscribe(current);
    if (!current->responded) CloseRequest(_bus, current->path);
    _request.reset();
    emit Failed(current->id, error, message);
}

void PortalScreenshotService::OnResponse(uint response, const QVariantMap& results, const QDBusMessage& message)
{
    const auto request = _request;
    if (!request || request->finished || request->responded || request->path != message.path()) return;
    request->responded = true;
    Unsubscribe(request);
    if (response == 1) { Fail(request, Error::Cancelled, "screenshot cancelled in the portal"); return; }
    if (response != 0) { Fail(request, Error::BackendFailure, "screenshot portal rejected the request"); return; }
    const auto value = results.value("uri");
    const QUrl uri(value.toString(), QUrl::StrictMode);
    if (value.metaType().id() != QMetaType::QString || !uri.isValid() || !uri.isLocalFile() ||
        !uri.host().isEmpty() || uri.hasQuery() || uri.hasFragment() || !QFileInfo(uri.toLocalFile()).isAbsolute()) {
        Fail(request, Error::InvalidResponse, "portal did not return a local screenshot URI"); return;
    }
    Decode(request, uri.toLocalFile());
}

void PortalScreenshotService::Decode(const std::shared_ptr<Request>& request, const QString& path)
{
    _decoding = true;
    auto* watcher = new QFutureWatcher<DecodedImage>(this);
    connect(watcher, &QFutureWatcher<DecodedImage>::finished, this, [this, watcher, request] {
        const auto result = watcher->result();
        watcher->deleteLater();
        _decoding = false;
        if (_request != request || request->finished) return;
        if (!result.error.isEmpty()) { Fail(request, Error::ImageReadFailed, result.error); return; }
        request->finished = true;
        _deadline.stop();
        _request.reset();
        emit Captured(request->id, result.image);
    });
    // Decode without blocking the rendering/event-loop thread; never borrow the service in a worker.
    watcher->setFuture(QtConcurrent::run([path] { return ReadImage(path); }));
}
