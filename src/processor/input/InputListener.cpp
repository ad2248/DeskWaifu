#include "InputListener.hpp"
#include <chrono>

InputListener::InputListener(ProcessorEventLoop& loop, IProcessor& interaction)
    : Processor(loop), _interaction(interaction)
{
    QObject::connect(&_cursorTimer, &QTimer::timeout, this, [this] { PollCursor(); });
}

bool InputListener::HandleEvent(const ProcessorEvent& event)
{
    if (event.type == "window.state") {
        _width = event.data.value("width").toInt(); _height = event.data.value("height").toInt();
        _output = event.data.value("output").toString().toStdString();
        return false;
    }
    if (event.type == "window.switch") { SetPaused(event.data.value("paused").toBool()); return false; }
    if (event.type == "input.native_move") {
        const auto sample = event.data.value("sample").value<InputSamplePtr>();
        if (sample) { sample->consumed = true; Move(sample->x, sample->y); }
        return true;
    }
    if (event.type == "input.native") {
        const auto kind = event.data.value("kind").toString();
        if (kind == "move") Move(event.data.value("x").toDouble(), event.data.value("y").toDouble());
        else Event(kind, event.data);
        return true;
    }
    return false;
}

bool InputListener::StartCursor()
{
    const bool available = _cursor.Start();
    _cursorTimer.start(16); // 独立监听，不依赖窗口呈现；启动失败后仍允许晚到的合成器恢复。
    return available;
}

void InputListener::Event(const QString& kind, QVariantMap data)
{
    if (_stopped || _paused) return;
    _move.reset(); _global.reset();
    data.insert("kind", kind);
    if (!data.contains("time")) data.insert("time", static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()));
    Send("input.event", &_interaction, std::move(data));
}

void InputListener::Move(double x, double y, bool cursor)
{
    if (_stopped || _paused) return;
    if (cursor) _move.reset(); else _global.reset();
    auto& slot = cursor ? _global : _move;
    if (!slot || slot->consumed) {
        slot = std::make_shared<InputSample>(InputSample{x, y, cursor, false});
        Send("input.move", &_interaction, {{"sample", QVariant::fromValue(slot)}});
    } else { slot->x = x; slot->y = y; }
}

void InputListener::PollCursor()
{
    if (_stopped || _paused) return;
    _cursor.Poll(_output);
    double x = 0, y = 0;
    if (_cursor.LocalPosition(_width, _height, x, y)) Move(x, y, true);
}

void InputListener::Stop()
{
    _stopped = true;
    _cursorTimer.stop();
    _loop.Unregister(this);
    _cursor.Stop();
    _move.reset(); _global.reset();
}

void InputListener::SetPaused(bool paused)
{
    if (_stopped || paused == _paused) return;
    _paused = paused;
    if (paused) {
        // 更换表面时丢弃旧窗口已排队的输入，不引入额外事件/窗口代号。
        _loop.Unregister(this);
        _loop.Register(this);
        _move.reset(); _global.reset();
    }
}
