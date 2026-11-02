#include "HotkeyPanel.hpp"

#include <QApplication>
#include <QHeaderView>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QComboBox>

HotkeyPanel::HotkeyPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    _message = new QLabel(tr("当前模型未配置快捷键。"), this);
    _message->setWordWrap(true);
    layout->addWidget(_message);
    _table = new QTableWidget(0, 5, this);
    _table->setObjectName(QStringLiteral("hotkeyTable"));
    _table->setHorizontalHeaderLabels({tr("名称"), tr("按键"), tr("动作"), tr("状态"), tr("操作")});
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(_table);
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this] { _pressed.clear(); });
}

HotkeyPanel::~HotkeyPanel()
{
    // QWidget 基类析构时会隐藏窗口并发出 focusChanged，
    // 此时 _pressed 等成员已被销毁，因此应在成员仍存活时断开应用信号。
    QObject::disconnect(qApp, nullptr, this, nullptr);
    qApp->removeEventFilter(this);
}

int HotkeyPanel::keyCode(const QString& name)
{
    if (name.size() == 2 && name[0] == QLatin1Char('N') && name[1].isDigit())
        return Qt::Key_0 + name[1].digitValue();
    const QHash<QString, int> aliases = {
        {"LeftControl", Qt::Key_Control}, {"RightControl", Qt::Key_Control}, {"Ctrl", Qt::Key_Control},
        {"LeftShift", Qt::Key_Shift}, {"RightShift", Qt::Key_Shift}, {"Shift", Qt::Key_Shift},
        {"LeftAlt", Qt::Key_Alt}, {"RightAlt", Qt::Key_Alt}, {"Alt", Qt::Key_Alt},
        {"Return", Qt::Key_Return}, {"Enter", Qt::Key_Return}, {"Escape", Qt::Key_Escape},
        {"Space", Qt::Key_Space}, {"Tab", Qt::Key_Tab},
        {"ArrowUp", Qt::Key_Up}, {"ArrowDown", Qt::Key_Down},
        {"ArrowLeft", Qt::Key_Left}, {"ArrowRight", Qt::Key_Right}
    };
    if (aliases.contains(name)) return aliases.value(name);
    if (name.size() == 1 && name[0].isLetterOrNumber()) return name.toUpper()[0].unicode();
    if (name.startsWith('F'))
    {
        bool ok = false;
        int number = name.mid(1).toInt(&ok);
        if (ok && number >= 1 && number <= 24) return Qt::Key_F1 + number - 1;
    }
    return 0;
}

void HotkeyPanel::setConnected(bool connected)
{
    _connected = connected;
    _table->setEnabled(connected);
    if (!connected)
    {
        _bindings.clear();
        _pressed.clear();
        _table->setRowCount(0);
        _message->setText(tr("连接已断开，快捷键不可用。"));
    }
}

void HotkeyPanel::setBindings(const QJsonArray& bindings, const QString& warning)
{
    _bindings.clear();
    _table->setRowCount(bindings.size());
    const QString message = warning == QStringLiteral("Invalid VTube Studio hotkey configuration")
        ? tr("VTube Studio 快捷键配置无效") : warning;
    _message->setText(!message.isEmpty() ? message : bindings.isEmpty()
        ? tr("当前模型未配置快捷键。") : tr("快捷键仅在此控制窗口处于活动状态时生效。"));
    QStringList reasons;
    QList<bool> keyboardSupported;
    const QHash<QString, QString> translatedReasons = {
        {QStringLiteral("Missing or duplicate hotkey ID"), tr("快捷键 ID 缺失或重复")},
        {QStringLiteral("Unsupported action"), tr("不支持的动作")},
        {QStringLiteral("Release-to-deactivate is not supported"), tr("不支持松开按键后取消激活")},
        {QStringLiteral("Expression file missing or invalid"), tr("表情文件缺失或无效")}
    };
    for (const auto& value : bindings)
    {
        const auto object = value.toObject();
        Binding binding{object["id"].toString(), {}, object["enabled"].toBool() && object["reason"].toString().isEmpty()};
        QString reason = object["reason"].toString();
        reason = translatedReasons.value(reason, reason);
        for (const auto& key : object["keys"].toArray())
        {
            int code = keyCode(key.toString());
            if (!code) reason = tr("不支持的按键：%1").arg(key.toString());
            else binding.keys.insert(code);
        }
        if (!object["enabled"].toBool()) reason = tr("已在模型配置中禁用");
        reasons.append(reason);
        keyboardSupported.append(binding.usable && reason.isEmpty() && !binding.keys.isEmpty());
        _bindings.append(binding);
    }
    for (int row = 0; row < _bindings.size(); ++row)
    {
        auto& binding = _bindings[row];
        for (int other = 0; other < _bindings.size(); ++other)
            if (row != other && keyboardSupported[row] && keyboardSupported[other]
                && binding.keys == _bindings[other].keys)
                reasons[row] = tr("快捷键绑定冲突");
    }
    for (int row = 0; row < _bindings.size(); ++row)
    {
        const auto object = bindings[row].toObject();
        auto& binding = _bindings[row];
        QStringList keys;
        for (const auto& key : object["keys"].toArray()) keys.append(key.toString());
        const QString state = object["active"].toBool() ? tr("已激活") : tr("未激活");
        const QString action = object["action"].toString() == QStringLiteral("ToggleExpression")
            ? tr("切换表情") : object["action"].toString();
        const QStringList cells = {object["name"].toString(), keys.join(" + "), action,
            reasons[row].isEmpty() ? state : reasons[row]};
        for (int column = 0; column < cells.size(); ++column)
        {
            auto* item = new QTableWidgetItem(cells[column]);
            item->setToolTip(cells[column]);
            _table->setItem(row, column, item);
        }
        auto* button = new QPushButton(tr("切换"), _table);
        button->setEnabled(binding.usable);
        connect(button, &QPushButton::clicked, this, [this, id = binding.id] {
            if (_connected) emit triggered(id);
        });
        _table->setCellWidget(row, 4, button);
        binding.usable = binding.usable && reasons[row].isEmpty() && !binding.keys.isEmpty();
    }
}

bool HotkeyPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::WindowDeactivate && watched == window()) _pressed.clear();
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || widget->window() != window()) return false;
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease
        && event->type() != QEvent::ShortcutOverride) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    const int code = key->key() == Qt::Key_Backtab ? Qt::Key_Tab : key->key();
    const bool tab = code == Qt::Key_Tab;
    if (event->type() == QEvent::KeyRelease)
    {
        if (!key->isAutoRepeat()) _pressed.remove(code);
        return tab;
    }
    bool bound = false;
    for (const auto& binding : _bindings) bound |= binding.usable && binding.keys.contains(code);
    const bool editing = qobject_cast<QLineEdit*>(widget) || qobject_cast<QTextEdit*>(widget)
        || qobject_cast<QPlainTextEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget)
        || qobject_cast<QComboBox*>(widget);
    if (!tab && editing) return false;
    if (event->type() == QEvent::ShortcutOverride)
    {
        if (tab || (bound && _connected && window()->isActiveWindow()))
        {
            event->accept();
            return true;
        }
        return false;
    }
    if (_connected && window()->isActiveWindow() && !key->isAutoRepeat())
    {
        _pressed.insert(code);
        auto chord = _pressed;
        if (key->modifiers() & Qt::ControlModifier) chord.insert(Qt::Key_Control);
        if (key->modifiers() & Qt::ShiftModifier) chord.insert(Qt::Key_Shift);
        if (key->modifiers() & Qt::AltModifier) chord.insert(Qt::Key_Alt);
        if (key->modifiers() & Qt::MetaModifier) chord.insert(Qt::Key_Meta);
        for (const auto& binding : _bindings)
            if (binding.usable && binding.keys == chord) emit triggered(binding.id);
    }
    return tab || (bound && _connected && window()->isActiveWindow());
}
