#include "HotkeyPanel.hpp"

#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtTest>

class HotkeyPanelTest : public QObject
{
    Q_OBJECT
    static QJsonObject binding(const QString& id, const QJsonArray& keys)
    {
        return {{"id", id}, {"name", id}, {"action", "ToggleExpression"},
            {"keys", keys}, {"enabled", true}, {"active", false}};
    }

private slots:
    void windowKeysAndTabNavigation()
    {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* panel = new HotkeyPanel(&window);
        auto* edit = new QLineEdit(&window);
        auto* button = new QPushButton("Focus target", &window);
        layout->addWidget(panel);
        layout->addWidget(edit);
        layout->addWidget(button);
        panel->setConnected(true);
        panel->setBindings({binding("watermark", {"Tab"}), binding("blush", {"N1"})});
        QSignalSpy spy(panel, &HotkeyPanel::triggered);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        edit->setFocus();
        QTest::keyClick(edit, Qt::Key_Tab);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last()[0].toString(), QString("watermark"));
        QCOMPARE(QApplication::focusWidget(), edit);
        QTest::keyClick(edit, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(QApplication::focusWidget(), edit);
        QCOMPARE(spy.count(), 1);
        QTest::keyClick(edit, Qt::Key_1);
        QCOMPARE(edit->text(), QString("1"));
        QCOMPARE(spy.count(), 1);
        button->setFocus();
        QTest::keyPress(button, Qt::Key_Tab);
        QCOMPARE(spy.count(), 2);
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, "\t", true);
        QApplication::sendEvent(button, &repeat);
        QCOMPARE(spy.count(), 2);
        QTest::keyRelease(button, Qt::Key_Tab);
        QTest::keyClick(button, Qt::Key_1);
        QCOMPARE(spy.count(), 3);
        panel->setBindings({});
        QTest::keyClick(button, Qt::Key_Tab);
        QTest::keyClick(button, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(QApplication::focusWidget(), button);
        QCOMPARE(spy.count(), 3);
    }
    void conflictsButtonsModelSwitchAndFocusLoss()
    {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* panel = new HotkeyPanel(&window);
        layout->addWidget(panel);
        panel->setConnected(true);
        panel->setBindings({binding("one", {"Tab"}), binding("two", {"Tab"}), binding("unknown", {"MouseBack"})});
        auto* table = panel->findChild<QTableWidget*>();
        QCOMPARE(table->rowCount(), 3);
        QSignalSpy spy(panel, &HotkeyPanel::triggered);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(table, Qt::Key_Tab);
        QCOMPARE(spy.count(), 0);
        auto* toggle = qobject_cast<QPushButton*>(table->cellWidget(0, 4));
        QVERIFY(toggle->isEnabled());
        toggle->click();
        QCOMPARE(spy.count(), 1);
        // An unsupported chord must not shadow an otherwise valid binding.
        panel->setBindings({binding("valid", {"Tab"}), binding("invalid", {"Tab", "MouseBack"})});
        QTest::keyClick(table, Qt::Key_Tab);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.last()[0].toString(), QString("valid"));
        spy.clear();
        panel->setBindings({binding("new-model", {"N2"})});
        QTest::keyClick(table, Qt::Key_Tab);
        QCOMPARE(spy.count(), 0);
        QTest::keyClick(table, Qt::Key_2);
        QCOMPARE(spy.count(), 1);
        QWidget other;
        other.show();
        other.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTest::keyClick(&other, Qt::Key_2);
        QTest::keyClick(table, Qt::Key_2);
        QCOMPARE(spy.count(), 1);
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        panel->setConnected(false);
        QTest::keyClick(table, Qt::Key_2);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(table->rowCount(), 0);
    }
    void simultaneousKeys()
    {
        HotkeyPanel panel;
        panel.setConnected(true);
        panel.setBindings({binding("chord", {"LeftControl", "N1"})});
        QSignalSpy spy(&panel, &HotkeyPanel::triggered);
        panel.show();
        panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        QTest::keyClick(&panel, Qt::Key_1);
        QCOMPARE(spy.count(), 0);
        QTest::keyClick(&panel, Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(HotkeyPanelTest)
#include "HotkeyPanelTest.moc"
