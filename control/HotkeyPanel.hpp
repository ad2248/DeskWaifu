#pragma once

#include <QJsonArray>
#include <QSet>
#include <QWidget>

class QLabel;
class QTableWidget;

class HotkeyPanel : public QWidget
{
    Q_OBJECT
public:
    explicit HotkeyPanel(QWidget* parent = nullptr);
    void setBindings(const QJsonArray& bindings, const QString& warning = {});
    void setConnected(bool connected);
    void resetInput() { _pressed.clear(); }
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
