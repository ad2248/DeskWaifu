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
    // 同步处理本状态机的待投递事件；状态配置发生变化时返回 true。
    // 不支持的事件返回 false，不启动嵌套应用事件循环。
    bool Send(Event event);
    // 返回当前叶子状态；状态机尚未就绪时返回 Uninitialized。
    State Current() const;
    // 判断是否处于运行父状态；切屏、关闭及初始化阶段均不属于运行态。
    bool Running() const;
    bool Normal() const;
    bool ScreenEditing() const;
    // 普通按压或屏幕编辑拖动期间返回 true。
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
