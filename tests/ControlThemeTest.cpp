#include "ui_MainWindow.h"
#include "HotkeyPanel.hpp"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

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

    void overviewContainsRefreshWithoutConnectionLabel()
    {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);

        QVERIFY(ui.overviewTab->isAncestorOf(ui.refreshButton));
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

    void followsApplicationTheme_data()
    {
        QTest::addColumn<QColor>("background");
        QTest::addColumn<QColor>("foreground");
        QTest::newRow("light") << QColor("#fafafa") << QColor("#202020");
        QTest::newRow("dark") << QColor("#303030") << QColor("#eeeeee");
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
                 static_cast<QWidget*>(ui.refreshButton),
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
