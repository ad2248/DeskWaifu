#pragma once

#include "processor/Processor.hpp"
#include "KWinCursorTracker.hpp"
#include "contracts/InputSample.hpp"
#include <QTimer>
#include <memory>

class InputListener final : public Processor
{
public:
    InputListener(ProcessorEventLoop& loop, IProcessorPlugin& interaction);
    bool HandleEvent(const ProcessorEvent&) override;
    bool StartCursor();
    const std::string& CursorError() const { return _cursor.Error(); }
    void Event(const QString& kind, QVariantMap data = {});
    void Move(double x, double y, bool cursor = false);
    void Stop() noexcept override;
    void SetPaused(bool paused);
private:
    void PollCursor();
    IProcessorPlugin& _interaction;
    int _width = 0, _height = 0;
    std::string _output;
    KWinCursorTracker _cursor;
    QTimer _cursorTimer;
    InputSamplePtr _move, _global;
    bool _stopped = false, _paused = false;
};
