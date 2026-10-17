#include "MainWindow.hpp"
#include "TransformEditor.hpp"
#include "ScreenCropInteraction.hpp"
#include "HoverOpacitySettings.hpp"
#include <QLabel>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QStyleOptionSlider>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QStatusBar>
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
    void hoverOpacitySavesOnRelease()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const int fd = open(QFile::encodeName(directory.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        const auto closeRuntime = qScopeGuard([fd] { close(fd); });
        const auto previousRuntime = qgetenv("XDG_RUNTIME_DIR");
        const auto restoreRuntime = qScopeGuard([previousRuntime] { qputenv("XDG_RUNTIME_DIR", previousRuntime); });
        const QString runtimePath = QStringLiteral("/proc/self/fd/%1").arg(fd);
        qputenv("XDG_RUNTIME_DIR", runtimePath.toLocal8Bit());
        const QString configPath = directory.path() + "/custom.json";
        const auto writeConfig = [&](const QByteArray& text) {
            QFile file(configPath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            return file.write(text) == text.size();
        };
        QVERIFY(writeConfig("{\"hover_opacity_enabled\":true,\"hover_opacity\":0.37,\"model_x\":2.5,\"extra\":\"keep\"}"));
        LAppConfig hoverConfig; hoverConfig.sourcePath = configPath.toStdString();
        HoverOpacitySettings settings(hoverConfig);
        QVERIFY(settings.Refresh().ok);
        WindowRuntime runtime;
        ApplicationService service(runtime);
        TransformEditor editor(service, hoverConfig);
        QVERIFY(editor.Initialize({}));
        QLocalServer server;
        QVERIFY(server.listen(runtimePath + "/deskwaifu.sock"));
        QList<QJsonObject> mutations;
        int queries = 0;
        bool disconnectAfterSave = false, supported = true;
        bool delayQuery = false;
        QByteArray delayedResponse;
        QLocalSocket* connection = nullptr;
        connect(&server, &QLocalServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            connection = socket;
            connect(socket, &QLocalSocket::readyRead, this, [&, socket] {
                while (socket->canReadLine()) {
                    const auto json = socket->readLine();
                    const auto input = QJsonDocument::fromJson(json).object();
                    const auto command = input.value("command").toString();
                    QJsonObject response{{"ok", true}};
                    if (command == "get_transform_settings") {
                        const auto transform = editor.Display().transform;
                        response.insert("x", transform.x);
                        response.insert("y", transform.y);
                        response.insert("zoom", transform.scale);
                        response.insert("surface_width", 1920);
                        response.insert("surface_height", 1080);
                        response.insert("editing", editor.Editing());
                    }
                    if (command == "get_hover_opacity" || command == "set_hover_opacity") {
                        AppResult result = AppResult::Failure("unknown command");
                        if (command == "get_hover_opacity") ++queries;
                        else mutations.append(input);
                        if (supported) {
                            result = command == "get_hover_opacity" ? settings.Refresh() : settings.Set(json.toStdString());
                            response.insert("enabled", settings.Enabled());
                            response.insert("opacity", settings.Opacity());
                        }
                        response.insert("ok", result.ok);
                        response.insert("error", QString::fromStdString(result.error));
                        if (delayQuery && command == "get_hover_opacity") {
                            delayQuery = false;
                            delayedResponse = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
                            return;
                        }
                        if (disconnectAfterSave && command == "set_hover_opacity") {
                            disconnectAfterSave = false;
                            socket->abort();
                            return;
                        }
                    }
                    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                    socket->flush();
                }
            });
        });
        MainWindow window;
        auto* respawn = window.findChild<QPushButton*>("respawnButton");
        QVERIFY(respawn);
        window.show(); window.activateWindow();
        window.findChild<QTabWidget*>("tabWidget")->setCurrentWidget(window.findChild<QWidget*>("transformTab"));
        auto* group = window.findChild<QWidget*>("hoverOpacityGroup");
        auto* slider = window.findChild<QSlider*>("hoverOpacitySlider");
        auto* enabled = window.findChild<QCheckBox*>("hoverOpacityEnabledCheck");
        auto* value = window.findChild<QLabel*>("hoverOpacityValueLabel");
        auto* status = window.findChild<QLabel*>("operationStatusLabel");
        auto* connectionStatus = window.findChild<QLabel*>("connectionStatusLabel");
        QTRY_VERIFY(respawn->isEnabled());
        QVERIFY(group && slider && enabled && value && status && connectionStatus);
        QVERIFY(!window.findChild<QLabel*>("hoverOpacityStatusLabel"));
        QVERIFY(window.statusBar()->isAncestorOf(status));
        QVERIFY(window.statusBar()->isAncestorOf(connectionStatus));
        QVERIFY(!group->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled(), 2000);
        QVERIFY(enabled->isChecked());
        QCOMPARE(slider->value(), 37);
        QCOMPARE(value->text(), QString("37%"));
        QVERIFY(!slider->hasTracking());
        QVERIFY(mutations.isEmpty()); // Queries must never echo settings back as writes.
        QVERIFY(status->text().isEmpty()); // Successful queries do not invent operation feedback.
        const auto connectedText = connectionStatus->text();
        QVERIFY(connectedText.startsWith("Connected"));
        window.setStatusMessage("<saved>");
        QCOMPARE(status->text(), QString("<saved>"));
        QCOMPARE(status->textFormat(), Qt::PlainText);
        QCOMPARE(status->toolTip(), status->text());
        QCOMPARE(connectionStatus->text(), connectedText);
        window.setStatusMessage("temporary", 30);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().isEmpty() && status->toolTip().isEmpty(), 500);
        QCOMPARE(connectionStatus->text(), connectedText);
        window.setStatusMessage("old timeout", 30);
        window.setStatusMessage("new status");
        QTest::qWait(60);
        QCOMPARE(status->text(), QString("new status"));
        // Long operation messages stay in the status bar, without expanding the
        // window or hiding the connection area. The tooltip retains all text.
        const auto originalSize = window.size();
        window.setStatusMessage(QString(3000, 'x'));
        QCoreApplication::processEvents();
        QCOMPARE(window.size(), originalSize);
        QVERIFY(connectionStatus->isVisible() && status->isVisible());
        QVERIFY(connectionStatus->geometry().right() < status->geometry().left());
        QCOMPARE(status->toolTip(), QString(3000, 'x'));
        window.setStatusMessage(QString());
        delayQuery = true;
        QTRY_VERIFY_WITH_TIMEOUT(!delayedResponse.isEmpty(), 3000);
        QStyleOptionSlider option;
        option.initFrom(slider);
        option.orientation = Qt::Horizontal;
        option.minimum = 0; option.maximum = 100;
        option.sliderPosition = option.sliderValue = slider->value();
        const auto handle = slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, handle.center());
        QVERIFY(slider->isSliderDown());
        QTest::mouseMove(slider, QPoint(slider->width() * 4 / 5, handle.center().y()));
        QTest::qWait(100);
        const int released = slider->sliderPosition();
        QVERIFY(released > 37);
        QCOMPARE(value->text(), QStringLiteral("%1%").arg(released));
        QCOMPARE(slider->value(), 37);
        QCOMPARE(settings.Opacity(), 0.37f);
        QVERIFY(mutations.isEmpty());
        // Release with an older query still in flight. Its response must not
        // restore 37% over the value waiting to be sent by the user's release.
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier,
            QPoint(slider->width() * 4 / 5, handle.center().y()));
        QCOMPARE(slider->value(), released);
        connection->write(delayedResponse);
        connection->flush();
        QTRY_COMPARE_WITH_TIMEOUT(mutations.size(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled(), 2000);
        QCOMPARE(settings.Opacity(), float(released / 100.0));
        QVERIFY(mutations.last().contains("opacity") && !mutations.last().contains("enabled"));
        QVERIFY(status->text().contains(QStringLiteral("已设置并保存")));
        QCOMPARE(connectionStatus->text(), connectedText);
        window.findChild<QTabWidget*>("tabWidget")->setCurrentIndex(0);
        QVERIFY(status->isVisible());
        window.findChild<QTabWidget*>("tabWidget")->setCurrentWidget(window.findChild<QWidget*>("transformTab"));
        const int beforeStatusPoll = queries;
        QTRY_VERIFY_WITH_TIMEOUT(queries > beforeStatusPoll, 2500);
        QTest::qWait(50);
        QCOMPARE(status->text(), QStringLiteral("已设置并保存"));
        QCOMPARE(connectionStatus->text(), connectedText);
        const auto screenshot = qEnvironmentVariable("DESKWAIFU_HOVER_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        ConfigStore store(configPath.toStdString());
        ConfigStore::Snapshot disk; std::string error;
        QVERIFY(store.Read(disk, error));
        QCOMPARE(disk.values.at("model_x"), std::string("2.5"));
        QVERIFY(!disk.values.count("extra"));
        QCOMPARE(disk.values.size(), size_t(10));
        QTest::keyClick(slider, Qt::Key_Home);
        QTRY_VERIFY_WITH_TIMEOUT(settings.Opacity() == 0 && group->isEnabled(), 2000);
        QTest::keyClick(slider, Qt::Key_End);
        QTRY_VERIFY_WITH_TIMEOUT(settings.Opacity() == 1 && group->isEnabled(), 2000);
        QVERIFY(editor.Begin("another-client", false, 1920, 1080).ok);
        auto draft = editor.Display();
        draft.transform.x = 4.5f;
        QVERIFY(editor.Update("another-client", draft).ok);
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QDoubleSpinBox*>("positionXSpin")->isEnabled(), 2000);
        QVERIFY(group->isEnabled()); // Independent of an active position/crop edit.
        enabled->click();
        QTRY_VERIFY_WITH_TIMEOUT(!settings.Enabled() && group->isEnabled(), 2000);
        QVERIFY(mutations.last().contains("enabled") && !mutations.last().contains("opacity"));
        QCOMPARE(settings.Opacity(), 1.f);
        QVERIFY(editor.Editing());
        QCOMPARE(service.Snapshot().transform.x, 4.5f);
        // Whole-document saves detect the independent hover write during editing.
        QVERIFY(!editor.Apply("another-client").ok);
        QVERIFY(editor.Cancel("another-client").ok);
        QCOMPARE(service.Snapshot().transform.x, 2.5f);
        editor.Reload(); // The backend periodically refreshes the comparison snapshot.
        QVERIFY(editor.Reset("another-client").ok);
        QVERIFY(store.Read(disk, error));
        QVERIFY(disk.values.count("hover_opacity") && disk.values.count("hover_opacity_enabled"));
        QVERIFY(!settings.Enabled() && settings.Opacity() == 1.f);
        const auto saved = QByteArray::fromStdString(disk.document);
        QVERIFY(writeConfig("{invalid"));
        slider->setValue(50);
        QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled() && status->text().contains("Invalid JSON"), 2000);
        QCOMPARE(settings.Opacity(), 1.f);
        QCOMPARE(slider->value(), 100); // Save failure restores the effective backend value.
        QCOMPARE(value->text(), QString("100%"));
        QVERIFY(writeConfig(saved));
        disconnectAfterSave = true;
        const int beforeDisconnect = mutations.size();
        slider->setValue(65);
        QTRY_COMPARE_WITH_TIMEOUT(settings.Opacity(), 0.65f, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(group->isEnabled() && queries > 1, 5000);
        QCOMPARE(slider->value(), 65);
        QCOMPARE(mutations.size(), beforeDisconnect + 1); // Lost acknowledgements are not replayed.
        QVERIFY(window.findChild<QPlainTextEdit*>("responseLog")->toPlainText().contains("unknown"));
        supported = false;
        const int beforeOldBackend = queries;
        connection->abort();
        QTRY_VERIFY(!respawn->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(queries > beforeOldBackend, 5000);
        QTRY_VERIFY(respawn->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!group->isEnabled() && status->text() == "unknown command", 2000);
    }

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
        LAppConfig config; config.sourcePath = path;
        TransformEditor editor(service, config); QVERIFY(editor.Initialize({}));
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
        auto* spawn = window.findChild<QPushButton*>("spawnButton");
        QVERIFY(spawn);
        QCOMPARE(spawn->text(), QString("Spawn"));
        QVERIFY(!window.findChild<QPushButton*>("refreshButton"));
        auto* connectionStatus = window.findChild<QLabel*>("connectionStatusLabel");
        auto* operationStatus = window.findChild<QLabel*>("operationStatusLabel");
        QVERIFY(connectionStatus && operationStatus);
        const auto connectedText = connectionStatus->text();
        spawn->click(); // A connected GUI must not start another backend.
        QVERIFY(window.findChild<QPlainTextEdit*>("responseLog")->toPlainText().contains("already connected"));
        QVERIFY(operationStatus->text().contains("already connected"));
        QCOMPARE(connectionStatus->text(), connectedText);
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
        QVERIFY(store.Read(disk, error)); QCOMPARE(disk.values.size(), size_t(10));
        QVERIFY(!disk.values.count("model_region"));
        QCOMPARE(disk.values.at("model_x"), std::string("0"));
        QCOMPARE(disk.values.at("model_y"), std::string("0"));
        QCOMPARE(disk.values.at("model_scale"), std::string("1"));
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
