#include "MainWindow.hpp"
#include "TransformEditor.hpp"
#include "ScreenCropInteraction.hpp"
#include <QLabel>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QTabWidget>
#include <QJsonDocument>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QtTest>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/un.h>
#include <unistd.h>

struct WindowRuntime : IApplicationRuntime
{
    bool SelectModel(int) override { return true; }
    int ModelIndex() const override { return 0; }
    int ModelCount() const override { return 1; }
    bool SetHidden(bool) override { return true; }
    bool PlayMotion(const std::string&, int, int) override { return true; }
    bool SetExpression(const std::string&) override { return true; }
    bool TriggerHotkey(const std::string&, std::string&) override { return true; }
    bool SwitchSkin() override { return true; }
};

class MainWindowTransformTest : public QObject
{
    Q_OBJECT
private slots:
    void previewsSaveCancelAndReset()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(directory.path().startsWith(QDir::tempPath() + "/"));
        if (qEnvironmentVariable("DESKWAIFU_EXPECT_LONG_TEMP") == QStringLiteral("ON")) {
            sockaddr_un address{};
            QVERIFY(QFile::encodeName(directory.path() + "/deskwaifu.sock").size() >= qsizetype(sizeof(address.sun_path)));
        }
        // Linux Unix socket names are limited to sun_path's size, even when
        // their containing directory is a valid (long) filesystem path. Keep
        // the socket in TMPDIR, but address it through this process's short
        // directory-fd alias. The descriptor outlives both server and GUI.
        const int runtimeFd = open(QFile::encodeName(directory.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY2(runtimeFd >= 0, std::strerror(errno));
        const auto closeRuntime = qScopeGuard([runtimeFd] { close(runtimeFd); });
        const QString runtimePath = QStringLiteral("/proc/self/fd/%1").arg(runtimeFd);
        qputenv("XDG_RUNTIME_DIR", runtimePath.toLocal8Bit());
        const auto path = (directory.path() + "/config.json").toStdString();
        WindowRuntime runtime; ApplicationService service(runtime);
        TransformEditor editor(service, path); QVERIFY(editor.Initialize({}));
        QLocalServer server;
        QVERIFY2(server.listen(runtimePath + "/deskwaifu.sock"), qPrintable(server.errorString()));
        connect(&server, &QLocalServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, this, [&, socket] {
                while (socket->canReadLine()) {
                    const auto input = QJsonDocument::fromJson(socket->readLine()).object();
                    const auto command = input.value("command").toString();
                    AppResult result = AppResult::Success();
                    if (command == "begin_transform_edit") result = editor.Begin("gui", input.value("screen").toBool(), 1920, 1080);
                    if (command == "update_transform_edit") {
                        auto settings = editor.Display();
                        settings.transform = {float(input.value("zoom").toDouble()), float(input.value("x").toDouble()), float(input.value("y").toDouble())};
                        settings.cropped = input.value("cropped").toBool();
                        settings.region = {input.value("crop_x").toInt(), input.value("crop_y").toInt(), input.value("crop_width").toInt(), input.value("crop_height").toInt()};
                        result = editor.Update("gui", settings);
                    }
                    if (command == "apply_transform_edit") result = editor.Apply("gui");
                    if (command == "cancel_transform_edit") result = editor.Cancel("gui");
                    if (command == "reset_transform_settings") result = editor.Reset("gui");
                    const auto settings = editor.Display();
                    QJsonObject response{{"ok", result.ok}, {"error", QString::fromStdString(result.error)},
                        {"x", settings.transform.x}, {"y", settings.transform.y}, {"zoom", settings.transform.scale},
                        {"cropped", settings.cropped}, {"crop_x", settings.region.x}, {"crop_y", settings.region.y},
                        {"crop_width", settings.region.width}, {"crop_height", settings.region.height},
                        {"surface_width", 1920}, {"surface_height", 1080}, {"owned", editor.OwnedBy("gui")},
                        {"editing", editor.Editing()}, {"local_screen_editing", editor.LocalScreenEditing()},
                        {"revision", int(editor.Revision())}, {"config_path", QString::fromStdString(path)}};
                    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                    socket->flush();
                }
            });
        });
        MainWindow window;
        window.show();
        window.findChild<QTabWidget*>("tabWidget")->setCurrentWidget(window.findChild<QWidget*>("transformTab"));
        auto* x = window.findChild<QDoubleSpinBox*>("positionXSpin");
        auto* zoom = window.findChild<QDoubleSpinBox*>("zoomSpin");
        auto* enabled = window.findChild<QCheckBox*>("cropEnabledCheck");
        auto* width = window.findChild<QSpinBox*>("cropBox_w");
        auto* apply = window.findChild<QPushButton*>("transformButton");
        auto* cancel = window.findChild<QPushButton*>("cancelTransformButton");
        auto* reset = window.findChild<QPushButton*>("xformResetButton");
        auto* screen = window.findChild<QPushButton*>("dragEditButton");
        QVERIFY(x && zoom && enabled && width && apply && cancel && reset && screen);
        QTRY_VERIFY_WITH_TIMEOUT(x->isEnabled(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!width->isEnabled(), 2000);
        QTest::qWait(100); // initial query has finished before the first user edit
        x->setValue(2.5); zoom->setValue(1.5); enabled->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 2000);
        QCOMPARE(width->value(), 1920);
        ConfigStore store(path); ConfigStore::Snapshot disk; std::string error;
        QVERIFY(store.Read(disk, error)); QVERIFY(disk.values.empty());
        apply->click();
        QTRY_VERIFY_WITH_TIMEOUT(!editor.Editing(), 2000);
        QVERIFY(store.Read(disk, error));
        QCOMPARE(QString::fromStdString(disk.values.at("model_x")), QString("2.5"));
        QVERIFY(disk.values.count("model_region"));
        QTRY_VERIFY_WITH_TIMEOUT(screen->isEnabled(), 2000);
        screen->click();
        QTRY_VERIFY_WITH_TIMEOUT(cancel->isEnabled(), 2000);
        const auto screenshot = qEnvironmentVariable("DESKWAIFU_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        x->setValue(3.5);
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 2000);
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(!editor.Editing(), 2000);
        QCOMPARE(service.Snapshot().transform.x, 2.5f);
        reset->click();
        QTRY_COMPARE_WITH_TIMEOUT(service.Snapshot().transform.x, 0.f, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!enabled->isChecked(), 2000);
        QCOMPARE(service.Snapshot().transform.scale, 1.f);
        QVERIFY(store.Read(disk, error)); QVERIFY(disk.values.empty());
        QVERIFY(!width->isEnabled());

        // Polling reflects edits initiated in the model window and unlocks the
        // panel when a frame double-click saves that transaction.
        ScreenCropInteraction pointer;
        QVERIFY(pointer.StartModelDrag(editor, 100, 100, 120, 100, 1920, 1080));
        QVERIFY(pointer.Release(editor, 100, 120, 100).ok);
        QTRY_VERIFY_WITH_TIMEOUT(!x->isEnabled(), 3000);
        QVERIFY(!apply->isEnabled() && !reset->isEnabled());
        QVERIFY(window.findChild<QLabel*>("transformStatusLabel")->text().contains(QStringLiteral("模型窗口")));
        for (uint32_t time : {200u, 300u}) {
            pointer.Press(editor.Display(), 0, 200);
            QVERIFY(pointer.Release(editor, time, 0, 200).ok);
        }
        QTRY_VERIFY_WITH_TIMEOUT(x->isEnabled(), 3000);
        QVERIFY(enabled->isChecked());
        QVERIFY(store.Read(disk, error) && disk.values.count("model_region"));
    }
};

QTEST_MAIN(MainWindowTransformTest)
#include "MainWindowTransformTest.moc"
