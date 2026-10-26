#include "ControlTestWindow.hpp"
#include "HotkeyPanel.hpp"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QAbstractItemView>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QtTest>

// 部分桌面主题让表单字段保持初始建议尺寸，此时内容只有“自动”。
class CompactFormStyle : public QProxyStyle
{
public:
    CompactFormStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
        const QWidget* widget = nullptr, QStyleHintReturn* data = nullptr) const override
    {
        if (hint == SH_FormLayoutFieldGrowthPolicy) return QFormLayout::FieldsStayAtSizeHint;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};

class ControlThemeTest : public QObject
{
    Q_OBJECT

    QPalette _originalPalette;
    QFont _originalFont;
    QTemporaryDir _runtime{QStringLiteral("/tmp/opencode/gui-theme-XXXXXX")};
    QByteArray _previousRuntime;

private slots:
    void initTestCase()
    {
        QVERIFY(_runtime.isValid());
        _previousRuntime = qgetenv("XDG_RUNTIME_DIR");
        qputenv("XDG_RUNTIME_DIR", _runtime.path().toLocal8Bit());
    }

    void cleanupTestCase()
    {
        qputenv("XDG_RUNTIME_DIR", _previousRuntime);
    }

    void panelUsesChineseText()
    {
        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);
        QCOMPARE(window.windowTitle(), QString("DeskWaifu 控制面板"));
        const QStringList tabs = {"系统", "交互", "模型", "语音", "日志"};
        QCOMPARE(ui.tabWidget->count(), tabs.size());
        for (int i = 0; i < tabs.size(); ++i)
            QCOMPARE(ui.tabWidget->tabText(i), tabs[i]);
        for (auto* button : window.findChildren<QPushButton*>())
            QVERIFY2(button->text().contains(QRegularExpression("[\\x{4e00}-\\x{9fff}]")), qPrintable(button->objectName()));
        for (auto* group : window.findChildren<QGroupBox*>())
            QVERIFY2(group->title().contains(QRegularExpression("[\\x{4e00}-\\x{9fff}]")), qPrintable(group->objectName()));
        QCOMPARE(ui.respawnButton->text(), QString("重启"));
        QVERIFY(ui.respawnButton->toolTip().contains("重新启动"));
        QVERIFY(ui.renderDeviceCombo->toolTip().contains("“重启”"));
        QCOMPARE(ui.responseLog->placeholderText(), QString("IPC 响应将显示在此处。"));
    }

    void controlsBelongToTheirCategory()
    {
        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);
        QCOMPARE(window.findChildren<QTabWidget*>().size(), 1);
        const QList<QStringList> categories = {
            {"spawnButton", "respawnButton", "statusGroup", "renderDeviceGroup"},
            {"mouseLookGroup", "lookGroup", "hoverOpacityGroup", "hotkeyGroup"},
            {"modelGroup", "toggleHiddenButton", "switchSkinButton", "expressionGroup", "motionGroup",
                "mouthGroup", "outputCombo", "transformGroup", "positionEdit", "transformActionsLayout", "transformStatusLabel"},
            {"speechSettingsGroup"},
            {"responseLog"}
        };
        for (int tab = 0; tab < categories.size(); ++tab) {
            for (const auto& name : categories[tab]) {
                auto* object = window.findChild<QObject*>(name);
                QVERIFY2(object, qPrintable(name));
                while (object && object != ui.tabWidget->widget(tab)) object = object->parent();
                QVERIFY2(object, qPrintable(name));
            }
        }
    }

    void pagesScrollAtMinimumWindowSize_data()
    {
        QTest::addColumn<int>("fontSize");
        QTest::newRow("normal") << 10;
        QTest::newRow("larger-text") << 13;
    }

    void pagesScrollAtMinimumWindowSize()
    {
        QFETCH(int, fontSize);
        QFont font = QApplication::font();
        font.setPointSize(fontSize);
        QApplication::setFont(font);
        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);
        window.resize(720, 560);
        window.show();
        const QStringList lastControls = {"renderDeviceStatus", "hoverOpacityHintLabel", "transformStatusLabel"};
        for (int tab = 0; tab < lastControls.size(); ++tab) {
            ui.tabWidget->setCurrentIndex(tab);
            QCoreApplication::processEvents();
            auto* page = ui.tabWidget->widget(tab);
            auto* scroll = qobject_cast<QScrollArea*>(page);
            auto* target = page->findChild<QWidget*>(lastControls[tab]);
            QVERIFY(scroll && target);
            QVERIFY(scroll->widgetResizable());
            scroll->ensureWidgetVisible(target);
            QCoreApplication::processEvents();
            QCOMPARE(window.size(), QSize(720, 560));
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            const QRect targetRect(target->mapTo(scroll->viewport(), QPoint()), target->size());
            QVERIFY2(scroll->viewport()->rect().contains(targetRect), qPrintable(lastControls[tab]));
        }
    }

    void init()
    {
        _originalPalette = QApplication::palette();
        _originalFont = QApplication::font();
    }

    void cleanup()
    {
        QApplication::setPalette(_originalPalette);
        QApplication::setFont(_originalFont);
    }

    void systemContainsSpawnWithoutConnectionLabel()
    {
        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);

        QVERIFY(ui.systemTab->isAncestorOf(ui.spawnButton));
        QCOMPARE(ui.spawnButton->text(), QString("启动"));
        QCOMPARE(ui.tabWidget->currentWidget(), ui.systemTab);
        QVERIFY(!window.findChild<QLabel*>(QStringLiteral("connectionLabel")));
        QCOMPARE(window.statusBar(), ui.statusBar);
    }

    void cropControlsUseIntegerSurfaceCoordinates()
    {
        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);

        for (auto* coordinate : {ui.cropBox_x, ui.cropBox_y})
        {
            QCOMPARE(coordinate->singleStep(), 1);
            coordinate->setValue(-1920);
            QCOMPARE(coordinate->value(), -1920);
            coordinate->setValue(3840);
            QCOMPARE(coordinate->value(), 3840);
        }
        for (auto* dimension : {ui.cropBox_w, ui.cropBox_h})
        {
            QCOMPARE(dimension->minimum(), 1);
            QCOMPARE(dimension->singleStep(), 1);
            dimension->setValue(3840);
            QCOMPARE(dimension->value(), 3840);
            dimension->setValue(0);
            QCOMPARE(dimension->value(), 1);
        }
    }

    void renderDeviceComboUsesAvailableWidth_data()
    {
        QTest::addColumn<int>("fontSize");
        QTest::newRow("normal") << 10;
        QTest::newRow("larger-text") << 13;
    }

    void renderDeviceComboUsesAvailableWidth()
    {
        QFETCH(int, fontSize);
        QFont font = _originalFont;
        font.setPointSize(fontSize);
        QApplication::setFont(font);
        CompactFormStyle style;
        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);
        ui.renderDeviceGroup->setStyle(&style);
        ui.renderDeviceCombo->setStyle(&style);
        window.resize(720, 560);
        window.show();
        QCoreApplication::processEvents(); // 初始建议尺寸只包含“自动”。
        ui.renderDeviceCombo->addItems({
            "Mesa Intel(R) Graphics (ARL) (0000:00:02.0)",
            "NVIDIA GeForce RTX 2070 SUPER (0000:02:00.0)"});
        ui.renderDeviceCombo->setEnabled(true);
        ui.renderDeviceCombo->setCurrentIndex(1);
        QCoreApplication::processEvents();
        QStyleOptionComboBox option;
        option.initFrom(ui.renderDeviceCombo);
        const auto textRect = ui.renderDeviceCombo->style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, ui.renderDeviceCombo);
        for (int i = 0; i < ui.renderDeviceCombo->count(); ++i) {
            const auto text = ui.renderDeviceCombo->itemText(i);
            QVERIFY2(textRect.width() >= ui.renderDeviceCombo->fontMetrics().horizontalAdvance(text), qPrintable(text));
        }
        ui.renderDeviceCombo->showPopup();
        QCoreApplication::processEvents();
        for (int i = 0; i < ui.renderDeviceCombo->count(); ++i)
            QVERIFY(ui.renderDeviceCombo->view()->viewport()->width() >=
                ui.renderDeviceCombo->view()->fontMetrics().horizontalAdvance(ui.renderDeviceCombo->itemText(i)));
        ui.renderDeviceCombo->hidePopup();
        const auto screenshot = qEnvironmentVariable("DESKWAIFU_DROPDOWN_SCREENSHOT");
        if (!screenshot.isEmpty() && fontSize == 13) QVERIFY(window.grab().save(screenshot));
        // 特别长的设备名可以省略显示，但不能使窗口变宽。
        ui.renderDeviceCombo->addItem(QString(600, 'W'));
        ui.renderDeviceCombo->setCurrentIndex(ui.renderDeviceCombo->count() - 1);
        QCoreApplication::processEvents();
        QCOMPARE(window.width(), 720);
    }

    void followsApplicationTheme_data()
    {
        QTest::addColumn<QColor>("background");
        QTest::addColumn<QColor>("foreground");
        QTest::newRow("light") << QColor("#fafafa") << QColor("#202020");
        QTest::newRow("dark") << QColor("#303030") << QColor("#eeeeee");
    }

    void hoverOpacityControlsLiveInInteractionTab()
    {
        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);
        QVERIFY(ui.interactionTab->isAncestorOf(ui.hoverOpacityGroup));
        QVERIFY(ui.hoverOpacityGroup->isAncestorOf(ui.hoverOpacityEnabledCheck));
        QVERIFY(ui.hoverOpacityGroup->isAncestorOf(ui.hoverOpacitySlider));
        QVERIFY(!ui.hoverOpacityGroup->isEnabled());
        QCOMPARE(ui.hoverOpacitySlider->orientation(), Qt::Horizontal);
        QCOMPARE(ui.hoverOpacitySlider->minimum(), 0);
        QCOMPARE(ui.hoverOpacitySlider->maximum(), 100);
        QCOMPARE(ui.hoverOpacitySlider->value(), 50);
        QVERIFY(!ui.hoverOpacitySlider->hasTracking());
        QVERIFY(ui.hoverOpacityGroup->isAncestorOf(ui.idleOpacitySlider));
        QCOMPARE(ui.idleOpacitySlider->minimum(), 0);
        QCOMPARE(ui.idleOpacitySlider->maximum(), 100);
        QCOMPARE(ui.idleOpacitySlider->value(), 100);
        QVERIFY(!ui.idleOpacitySlider->hasTracking());
    }

    void followsApplicationTheme()
    {
        QFETCH(QColor, background);
        QFETCH(QColor, foreground);
        QPalette palette = _originalPalette;
        palette.setColor(QPalette::Window, background);
        palette.setColor(QPalette::Button, background);
        palette.setColor(QPalette::Base, background);
        palette.setColor(QPalette::WindowText, foreground);
        palette.setColor(QPalette::ButtonText, foreground);
        palette.setColor(QPalette::Text, foreground);
        QFont font = _originalFont;
        font.setPointSize(13);
        QApplication::setPalette(palette);
        QApplication::setFont(font);

        ControlTestWindow window;
        ControlTestWidgets ui;
        ui.setupUi(&window);
        // Test this widget's active-row palette without casting a plugin-owned
        // class across the DSO boundary. Loaded widgets are checked below too.
        auto* hotkeys = new HotkeyPanel(ui.hotkeyGroup);
        hotkeys->setConnected(true);
        hotkeys->setBindings({QJsonObject{
            {"id", "expression"}, {"name", "Expression"},
            {"action", "ToggleExpression"}, {"keys", QJsonArray{"N1"}},
            {"enabled", true}, {"active", false}}});
        window.ensurePolished();

        QVERIFY(window.styleSheet().isEmpty());
        QCOMPARE(window.style(), QApplication::style());
        for (auto* widget : window.findChildren<QWidget*>())
        {
            QVERIFY2(widget->styleSheet().isEmpty(), qPrintable(widget->objectName()));
            QCOMPARE(widget->style(), QApplication::style());
        }
        for (QWidget* widget : {static_cast<QWidget*>(&window),
                 static_cast<QWidget*>(ui.spawnButton),
                 static_cast<QWidget*>(ui.statusBar),
                 static_cast<QWidget*>(ui.currentModelLabel),
                 static_cast<QWidget*>(ui.modelCombo),
                 static_cast<QWidget*>(ui.responseLog)})
        {
            QCOMPARE(widget->palette().color(QPalette::Window), background);
            QCOMPARE(widget->palette().color(QPalette::WindowText), foreground);
            QCOMPARE(widget->font(), font);
        }
    }
};

QTEST_MAIN(ControlThemeTest)
#include "ControlThemeTest.moc"
