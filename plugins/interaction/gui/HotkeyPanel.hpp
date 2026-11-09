#pragma once

#include <QJsonArray>
#include <QSet>
#include <QWidget>

class QLabel;
class QTableWidget;

// 展示模型快捷键，并在所属窗口活动时将按键组合转换为触发信号。
// 在 GUI 线程使用；构造和析构期间 QApplication 必须存活。
class HotkeyPanel : public QWidget
{
    Q_OBJECT
public:
    explicit HotkeyPanel(QWidget* parent = nullptr);
    ~HotkeyPanel() override;
    // 替换绑定列表，显示不可用原因及冲突；warning 非空时优先显示警告。
    void setBindings(const QJsonArray& bindings, const QString& warning = {});
    // 断开时清空绑定和按键状态，禁用触发控件。
    void setConnected(bool connected);
    void resetInput() { _pressed.clear(); }
    // 将配置中的按键名转换为 Qt 键码；不支持的名称返回 0。
    static int keyCode(const QString& name);

signals:
    void triggered(const QString& id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Binding { QString id; QSet<int> keys; bool usable; };
    QList<Binding> _bindings;
    QSet<int> _pressed;
    QLabel* _message;
    QTableWidget* _table;
    bool _connected = false;
};
