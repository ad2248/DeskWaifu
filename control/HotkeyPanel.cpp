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
    _message = new QLabel(tr("No model hotkeys configured."), this);
    _message->setWordWrap(true);
    layout->addWidget(_message);
    _table = new QTableWidget(0, 5, this);
    _table->setObjectName(QStringLiteral("hotkeyTable"));
    _table->setHorizontalHeaderLabels({tr("Name"), tr("Keys"), tr("Action"), tr("State"), tr("Operation")});
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(_table);
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this] { _pressed.clear(); });
}

HotkeyPanel::~HotkeyPanel()
{
    // QWidget's base destructor hides the window and emits focusChanged
    // after members such as _pressed are already destroyed. Disconnect the
    // application signal while those members are still alive.
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
        _message->setText(tr("Disconnected. Hotkeys are unavailable."));
    }
}

void HotkeyPanel::setBindings(const QJsonArray& bindings, const QString& warning)
{
    _bindings.clear();
    _table->setRowCount(bindings.size());
    _message->setText(!warning.isEmpty() ? warning : bindings.isEmpty()
        ? tr("No model hotkeys configured.") : tr("Hotkeys respond only while this control window is active."));
    QStringList reasons;
    QList<bool> keyboardSupported;
    for (const auto& value : bindings)
    {
        const auto object = value.toObject();
        Binding binding{object["id"].toString(), {}, object["enabled"].toBool() && object["reason"].toString().isEmpty()};
        QString reason = object["reason"].toString();
        for (const auto& key : object["keys"].toArray())
        {
            int code = keyCode(key.toString());
            if (!code) reason = tr("Unsupported key: %1").arg(key.toString());
            else binding.keys.insert(code);
        }
        if (!object["enabled"].toBool()) reason = tr("Disabled in model configuration");
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
                reasons[row] = tr("Conflicting key binding");
    }
    for (int row = 0; row < _bindings.size(); ++row)
    {
        const auto object = bindings[row].toObject();
        auto& binding = _bindings[row];
        QStringList keys;
        for (const auto& key : object["keys"].toArray()) keys.append(key.toString());
        const QString state = object["active"].toBool() ? tr("Active") : tr("Inactive");
        const QStringList cells = {object["name"].toString(), keys.join(" + "), object["action"].toString(),
            reasons[row].isEmpty() ? state : reasons[row]};
        for (int column = 0; column < cells.size(); ++column)
        {
            auto* item = new QTableWidgetItem(cells[column]);
            item->setToolTip(cells[column]);
            _table->setItem(row, column, item);
        }
        auto* button = new QPushButton(tr("Toggle"), _table);
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
