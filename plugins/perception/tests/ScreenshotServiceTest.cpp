#include "processor/screenshot/IScreenshotService.hpp"
#include "processor/screenshot/portal/PortalScreenshotService.hpp"
#include <QCoreApplication>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
const QString PortalService = "org.freedesktop.portal.Desktop";
const QString DesktopPath = "/org/freedesktop/portal/desktop";
const QString RequestInterface = "org.freedesktop.portal.Request";

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void Wait(const std::function<bool()>& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    Check(predicate(), "event-loop wait timed out");
}

class FakeRequest : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    explicit FakeRequest(QObject* parent) : QObject(parent) {}
public slots:
    void Close() { emit Closed(); }
signals:
    void Closed();
};

class FakePortal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
public:
    enum class Mode { Success, Cancelled, Rejected, MissingUri, RemoteUri, MissingFile,
        BrokenImage, LargeImage, NoResponse, EarlyResponse, DelayedReply, DifferentHandle,
        DuplicateResponse, MethodError, BadHandle };
    QDBusConnection bus;
    QTemporaryDir files;
    QString goodFile, badFile, largeFile;
    QStringList paths;
    Mode mode = Mode::Success;
    int calls = 0, closes = 0;
    bool optionsValid = true;
    QString parent;

    explicit FakePortal(QDBusConnection connection) : bus(connection)
    {
        goodFile = files.filePath(QString::fromUtf8("test 截图 #%.png"));
        badFile = files.filePath("broken.png");
        largeFile = files.filePath("large.png");
        QImage image(7, 5, QImage::Format_ARGB32);
        image.fill(QColor(12, 34, 56, 255));
        Check(image.save(goodFile), "write synthetic image");
        QImage large(16385, 1, QImage::Format_ARGB32);
        large.fill(Qt::white); Check(large.save(largeFile), "write oversized synthetic image");
        QFile bad(badFile); Check(bad.open(QIODevice::WriteOnly), "write invalid image"); bad.write("not an image");
        Check(bus.registerService(PortalService), "register private fake portal service");
        Check(bus.registerObject(DesktopPath, this, QDBusConnection::ExportAllSlots), "register private fake portal");
    }

    ~FakePortal() override
    {
        for (const auto& path : paths) bus.unregisterObject(path);
        bus.unregisterObject(DesktopPath);
        bus.unregisterService(PortalService);
    }

    void AddRequest(const QString& path)
    {
        auto* request = new FakeRequest(this);
        connect(request, &FakeRequest::Closed, this, [this] { ++closes; });
        Check(bus.registerObject(path, request, QDBusConnection::ExportAllSlots), "register fake request");
        paths.append(path);
    }

    void Respond(const QString& path, uint code = 0, QVariantMap results = {})
    {
        auto signal = QDBusMessage::createSignal(path, RequestInterface, "Response");
        signal << code << results;
        Check(bus.send(signal), "send fake portal response");
    }

public slots:
    QDBusObjectPath Screenshot(const QString& parentWindow, const QVariantMap& options)
    {
        ++calls;
        parent = parentWindow;
        const auto token = options.value("handle_token").toString();
        optionsValid = optionsValid && token.startsWith("deskwaifu_") &&
            options.value("interactive").toBool() && options.value("modal").toBool();
        QString sender = message().service();
        sender.remove(':'); sender.replace('.', '_');
        QString path = DesktopPath + "/request/" + sender + '/' + token;
        if (mode == Mode::MethodError) {
            sendErrorReply(QDBusError::Failed, "test portal method error");
            return QDBusObjectPath("/");
        }
        if (mode == Mode::BadHandle) return QDBusObjectPath("/");
        if (mode == Mode::DifferentHandle) path += "_alternate";
        if (mode == Mode::DelayedReply) {
            setDelayedReply(true);
            const auto call = message();
            QTimer::singleShot(80, this, [this, path, call] {
                AddRequest(path);
                bus.send(call.createReply(QVariant::fromValue(QDBusObjectPath(path))));
            });
            return QDBusObjectPath("/");
        }
        AddRequest(path);
        if (mode == Mode::NoResponse) return QDBusObjectPath(path);
        uint code = 0;
        QVariantMap results{{"uri", QUrl::fromLocalFile(goodFile).toString(QUrl::FullyEncoded)}};
        switch (mode) {
        case Mode::Cancelled: code = 1; break;
        case Mode::Rejected: code = 2; break;
        case Mode::MissingUri: results.clear(); break;
        case Mode::RemoteUri: results["uri"] = "https://example.invalid/screenshot.png"; break;
        case Mode::MissingFile: results["uri"] = QUrl::fromLocalFile(files.filePath("missing.png")).toString(); break;
        case Mode::BrokenImage: results["uri"] = QUrl::fromLocalFile(badFile).toString(); break;
        case Mode::LargeImage: results["uri"] = QUrl::fromLocalFile(largeFile).toString(); break;
        default: break;
        }
        if (mode == Mode::EarlyResponse) Respond(path, code, results);
        else QTimer::singleShot(10, this, [this, path, code, results] { Respond(path, code, results); });
        if (mode == Mode::DuplicateResponse)
            QTimer::singleShot(20, this, [this, path, code, results] { Respond(path, code, results); });
        return QDBusObjectPath(path);
    }
};

struct Observer
{
    QString id;
    QImage image;
    IScreenshotService::Error error = IScreenshotService::Error::BackendFailure;
    int successes = 0, failures = 0;
    explicit Observer(IScreenshotService& service)
    {
        QObject::connect(&service, &IScreenshotService::Captured, &service, [this](const QString& request, const QImage& capture) {
            id = request; image = capture; ++successes;
        });
        QObject::connect(&service, &IScreenshotService::Failed, &service, [this](const QString& request, IScreenshotService::Error code, const QString& message) {
            Check(!message.isEmpty(), "error message"); id = request; error = code; ++failures;
        });
    }
    int Count() const { return successes + failures; }
};

struct PausedDecoder
{
    QThreadPool* pool = QThreadPool::globalInstance();
    int previousThreads = pool->maxThreadCount();
    QSemaphore entered, released;
    QFuture<void> blocker;
    bool resumed = false;
    PausedDecoder()
    {
        pool->setMaxThreadCount(1);
        blocker = QtConcurrent::run([this] { entered.release(); released.acquire(); });
        Check(entered.tryAcquire(1, 2000), "pause decoder pool");
    }
    void Resume() { if (!resumed) { resumed = true; released.release(); } }
    ~PausedDecoder() { Resume(); blocker.waitForFinished(); pool->setMaxThreadCount(previousThreads); }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(qEnvironmentVariable("DESKWAIFU_SCREENSHOT_TEST_BUS") == "1", "run only under private dbus-run-session test wrapper");
    const auto server = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "screenshot-test-server");
    const auto client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "screenshot-test-client");
    FakePortal portal(server);
    {
        auto service = CreateScreenshotService();
        Check(!service->Busy() && portal.calls == 0, "factory construction must not capture");
    }
    for (auto mode : {FakePortal::Mode::Success, FakePortal::Mode::EarlyResponse,
         FakePortal::Mode::DifferentHandle, FakePortal::Mode::DuplicateResponse}) {
        portal.mode = mode;
        PortalScreenshotService service(client, 1000);
        Observer observed(service);
        const auto id = service.Capture("wayland:test-parent");
        Check(!id.isEmpty() && service.Busy() && observed.Count() == 0, "capture must be asynchronous");
        Check(service.Capture().isEmpty(), "single request, no implicit queue");
        Wait([&] { return observed.Count() != 0; });
        Check(observed.id == id && observed.successes == 1 && observed.failures == 0, "successful result correlation");
        Check(!service.Busy(), "completed request must release service");
        Check(observed.image.size() == QSize(7, 5) && observed.image.pixelColor(0, 0) == QColor(12, 34, 56), "decoded synthetic pixels");
        Check(QFile::exists(portal.goodFile), "must not delete portal-owned files");
        Check(portal.optionsValid && portal.parent == "wayland:test-parent", "interactive options and optional parent");
        Check(!service.Cancel(id), "cannot cancel completed request");
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 30) { QCoreApplication::processEvents(); QThread::msleep(1); }
        Check(observed.Count() == 1, "duplicate response must not produce duplicate results");
    }
    for (const auto& item : {
        std::make_pair(FakePortal::Mode::Cancelled, IScreenshotService::Error::Cancelled),
        std::make_pair(FakePortal::Mode::Rejected, IScreenshotService::Error::BackendFailure),
        std::make_pair(FakePortal::Mode::MissingUri, IScreenshotService::Error::InvalidResponse),
        std::make_pair(FakePortal::Mode::RemoteUri, IScreenshotService::Error::InvalidResponse),
        std::make_pair(FakePortal::Mode::MissingFile, IScreenshotService::Error::ImageReadFailed),
        std::make_pair(FakePortal::Mode::BrokenImage, IScreenshotService::Error::ImageReadFailed),
        std::make_pair(FakePortal::Mode::LargeImage, IScreenshotService::Error::ImageReadFailed),
        std::make_pair(FakePortal::Mode::MethodError, IScreenshotService::Error::BackendFailure),
        std::make_pair(FakePortal::Mode::BadHandle, IScreenshotService::Error::InvalidResponse)}) {
        portal.mode = item.first;
        PortalScreenshotService service(client, 1000);
        Observer observed(service);
        const auto id = service.Capture();
        Wait([&] { return observed.Count() != 0; });
        Check(observed.id == id && observed.failures == 1 && observed.error == item.second && !service.Busy(), "typed error result");
        portal.mode = FakePortal::Mode::Success;
        const auto retry = service.Capture();
        Wait([&] { return observed.Count() == 2; });
        Check(observed.id == retry && observed.successes == 1, "recovery after request error");
    }
    {
        PortalScreenshotService service(QDBusConnection("nonexistent-screenshot-test-bus"), 1000);
        Observer observed(service);
        service.Capture();
        Check(observed.Count() == 0, "unavailable bus must fail asynchronously");
        Wait([&] { return observed.Count() == 1; });
        Check(observed.error == IScreenshotService::Error::Unavailable, "unavailable bus error");
    }
    {
        portal.mode = FakePortal::Mode::NoResponse;
        const auto previousCloses = portal.closes;
        PortalScreenshotService service(client, 50);
        Observer observed(service);
        service.Capture();
        Wait([&] { return observed.Count() == 1; });
        Check(observed.error == IScreenshotService::Error::TimedOut && !service.Busy(), "response timeout");
        Wait([&] { return portal.closes > previousCloses; });
    }
    {
        portal.mode = FakePortal::Mode::NoResponse;
        PortalScreenshotService service(client, 1000);
        Observer observed(service);
        const auto calls = portal.calls;
        const auto first = service.Capture();
        Wait([&] { return portal.calls > calls; });
        const auto oldPath = portal.paths.last();
        Check(!service.Cancel("wrong-id") && service.Busy(), "stale ID cannot cancel active capture");
        Check(service.Cancel(first) && observed.failures == 1 && observed.error == IScreenshotService::Error::Cancelled, "explicit cancellation");
        portal.mode = FakePortal::Mode::Success;
        const auto next = service.Capture();
        portal.Respond(oldPath, 0, {{"uri", QUrl::fromLocalFile(portal.goodFile).toString()}});
        Wait([&] { return observed.Count() == 2; });
        Check(observed.id == next && observed.successes == 1, "late old response must not complete a new request");
    }
    for (bool destroy : {false, true}) {
        portal.mode = FakePortal::Mode::Success;
        PausedDecoder paused;
        auto service = std::make_unique<PortalScreenshotService>(client, 1000);
        Observer observed(*service);
        const auto id = service->Capture();
        Wait([&] { return service->findChild<QFutureWatcherBase*>() != nullptr; });
        if (destroy) service.reset();
        else {
            Check(service->Cancel(id), "cancel queued decode");
            Check(service->Busy() && service->Capture().isEmpty(), "cancelled decode must not accumulate more workers");
        }
        paused.Resume();
        if (destroy) {
            Check(paused.pool->waitForDone(2000), "decode can finish after service destruction");
            Check(observed.Count() == 0, "destroyed service must not emit result");
        } else {
            Wait([&] { return !service->Busy(); });
            Check(observed.failures == 1 && observed.successes == 0, "discard cancelled decode result");
            const auto retry = service->Capture();
            Wait([&] { return observed.Count() == 2; });
            Check(observed.id == retry && observed.successes == 1, "capture after decoder cleanup");
        }
    }
    for (bool destroy : {false, true}) {
        portal.mode = FakePortal::Mode::DelayedReply;
        const auto calls = portal.calls, closes = portal.closes;
        auto service = std::make_unique<PortalScreenshotService>(client, 1000);
        const auto id = service->Capture();
        Wait([&] { return portal.calls > calls; });
        if (destroy) service.reset();
        else Check(service->Cancel(id), "cancel before delayed handle reply");
        Wait([&] { return portal.closes > closes; });
    }
    std::cout << "Screenshot interface and private portal lifecycle tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}

#include "ScreenshotServiceTest.moc"
