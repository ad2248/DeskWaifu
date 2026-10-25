#include "ui_MainWindow.h"
#include "HotkeyPanel.hpp"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QAbstractItemView>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionComboBox>
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

private slots:
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

    void overviewContainsSpawnWithoutConnectionLabel()
    {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);

        QVERIFY(ui.overviewTab->isAncestorOf(ui.spawnButton));
        QCOMPARE(ui.spawnButton->text(), QString("Spawn"));
        QCOMPARE(ui.tabWidget->currentWidget(), ui.overviewTab);
        QVERIFY(!window.findChild<QLabel*>(QStringLiteral("connectionLabel")));
        QCOMPARE(window.statusBar(), ui.statusBar);
    }

    void cropControlsUseIntegerSurfaceCoordinates()
    {
        QMainWindow window;
        Ui::MainWindow ui;
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
        QMainWindow window;
        Ui::MainWindow ui;
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

    void hoverOpacityControlsLiveInTransformTab()
    {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        QVERIFY(ui.transformTab->isAncestorOf(ui.hoverOpacityGroup));
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

        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        auto* hotkeys = new HotkeyPanel(&window);
        ui.tabWidget->insertTab(2, hotkeys, "Hotkeys");
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
