#pragma once

#include "processor/Processor.hpp"
#include "KWinCursorTracker.hpp"
#include "DesktopWindow.hpp"
#include <QTimer>
#include <memory>

// 连续移动合并为最新采样；离散按键/按钮作为顺序屏障，不丢失手势顺序。
struct InputSample
{
    double x = 0, y = 0;
    bool cursor = false, consumed = false;
};
using InputSamplePtr = std::shared_ptr<InputSample>;
Q_DECLARE_METATYPE(InputSamplePtr)

class InputListener final : public Processor
{
public:
    InputListener(ProcessorEventLoop& loop, IProcessor& interaction, DesktopWindow& window);
    bool HandleEvent(const ProcessorEvent&) override { return false; }
    bool StartCursor();
    const std::string& CursorError() const { return _cursor.Error(); }
    void Event(const QString& kind, QVariantMap data = {});
    void Move(double x, double y, bool cursor = false);
    void Stop();
    void SetPaused(bool paused);
private:
    void PollCursor();
    IProcessor& _interaction;
    DesktopWindow& _window;
    KWinCursorTracker _cursor;
    QTimer _cursorTimer;
    InputSamplePtr _move, _global;
    bool _stopped = false, _paused = false;
};
