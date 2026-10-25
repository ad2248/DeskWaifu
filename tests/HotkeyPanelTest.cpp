#include "HotkeyPanel.hpp"

#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
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
    void chineseLabelsAndStates()
    {
        HotkeyPanel panel;
        auto* table = panel.findChild<QTableWidget*>();
        auto* message = panel.findChild<QLabel*>();
        QCOMPARE(message->text(), QString("当前模型未配置快捷键。"));
        const QStringList headers = {"名称", "按键", "动作", "状态", "操作"};
        for (int i = 0; i < headers.size(); ++i)
            QCOMPARE(table->horizontalHeaderItem(i)->text(), headers[i]);
        panel.setConnected(true);
        auto inactive = binding("原始名称", {"N1"});
        auto active = binding("active", {"N2"});
        active["active"] = true;
        panel.setBindings({inactive, active});
        QCOMPARE(table->item(0, 0)->text(), QString("原始名称"));
        QCOMPARE(table->item(0, 1)->text(), QString("N1"));
        QCOMPARE(table->item(0, 2)->text(), QString("切换表情"));
        QCOMPARE(table->item(0, 3)->text(), QString("未激活"));
        QCOMPARE(table->item(1, 3)->text(), QString("已激活"));
        QCOMPARE(qobject_cast<QPushButton*>(table->cellWidget(0, 4))->text(), QString("切换"));
        QVERIFY(message->text().contains("活动状态"));
        inactive["reason"] = "Expression file missing or invalid";
        panel.setBindings({inactive}, "Invalid VTube Studio hotkey configuration");
        QCOMPARE(table->item(0, 3)->text(), QString("表情文件缺失或无效"));
        QCOMPARE(message->text(), QString("VTube Studio 快捷键配置无效"));
        panel.setConnected(false);
        QCOMPARE(message->text(), QString("连接已断开，快捷键不可用。"));
    }

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
        // 不支持的组合键不能遮蔽原本有效的绑定。
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

    void windowDestructionUnregistersFilter()
    {
        QWidget survivor;
        survivor.show();
        for (int index = 0; index < 16; ++index)
        {
            auto* panel = new HotkeyPanel;
            panel->setConnected(true);
            panel->setBindings({binding("repeat", {"Tab"})});
            panel->show();
            panel->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(panel));
            QTest::keyClick(panel, Qt::Key_Tab);
            delete panel;
            survivor.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&survivor));
            QTest::keyClick(&survivor, Qt::Key_Tab);
        }
    }
};

QTEST_MAIN(HotkeyPanelTest)
#include "HotkeyPanelTest.moc"
