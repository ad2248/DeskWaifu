#pragma once

#include <QStateMachine>
#include <array>

// 仅负责窗口策略；手势坐标和编辑事务仍由原对象持有。
// 所有调用均在应用的 Qt 线程执行。
class ModelWindowState : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Uninitialized, Initializing, NormalIdle, Pressed, EditIdle,
        DraggingModel, MovingCrop, ResizingCrop, PanelEditing,
        SwitchingOutput, Closing, Closed, Count
    };
    enum class Event {
        Initialize, Initialized, Close, Released, SwitchOutput, OutputReady,
        NormalMode, ScreenMode, PanelMode, Press, Release,
        DragModel, MoveCrop, ResizeCrop, CancelGesture
    };

    ModelWindowState();
    bool Send(Event event);
    State Current() const;
    bool Running() const;
    bool Normal() const;
    bool ScreenEditing() const;
    bool GestureActive() const;

signals:
    void InteractionCancelled();

private:
    QStateMachine _machine;
    QState* _running;
    QState* _normal;
    QState* _screen;
    std::array<QState*, static_cast<std::size_t>(State::Count)> _states{};
};
