#include "ModelWindowState.hpp"
#include <QAbstractTransition>
#include <QCoreApplication>

namespace {
const QEvent::Type PolicyEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
struct PolicyEvent : QEvent
{
    explicit PolicyEvent(ModelWindowState::Event value) : QEvent(PolicyEventType), value(value) {}
    ModelWindowState::Event value;
};
class PolicyTransition : public QAbstractTransition
{
public:
    PolicyTransition(ModelWindowState::Event value, QState* source, QState* target)
        : QAbstractTransition(source), _value(value) { setTargetState(target); }
protected:
    bool eventTest(QEvent* event) override
    {
        return event->type() == PolicyEventType && static_cast<PolicyEvent*>(event)->value == _value;
    }
    void onTransition(QEvent*) override {}
private:
    ModelWindowState::Event _value;
};
}

ModelWindowState::ModelWindowState()
{
    _running = new QState(&_machine);
    _normal = new QState(_running);
    _screen = new QState(_running);
    auto state = [this](State id, QState* parent) {
        auto* value = new QState(parent);
        _states[static_cast<std::size_t>(id)] = value;
        return value;
    };
    auto* uninitialized = state(State::Uninitialized, &_machine);
    auto* initializing = state(State::Initializing, &_machine);
    auto* idle = state(State::NormalIdle, _normal);
    auto* pressed = state(State::Pressed, _normal);
    auto* editIdle = state(State::EditIdle, _screen);
    auto* drag = state(State::DraggingModel, _screen);
    auto* move = state(State::MovingCrop, _screen);
    auto* resize = state(State::ResizingCrop, _screen);
    auto* panel = state(State::PanelEditing, _running);
    auto* switching = state(State::SwitchingOutput, &_machine);
    auto* closing = state(State::Closing, &_machine);
    auto* closed = state(State::Closed, &_machine);
    _machine.setInitialState(uninitialized);
    _running->setInitialState(_normal);
    _normal->setInitialState(idle);
    _screen->setInitialState(editIdle);
    auto transition = [](QState* source, Event event, QState* target) {
        return new PolicyTransition(event, source, target);
    };
    transition(uninitialized, Event::Initialize, initializing);
    transition(initializing, Event::Initialized, _running);
    for (auto* source : {uninitialized, initializing, _running, switching})
        transition(source, Event::Close, closing);
    transition(closing, Event::Released, closed);
    transition(_running, Event::SwitchOutput, switching);
    transition(switching, Event::OutputReady, _running);
    for (auto* source : {_normal, _screen, panel}) {
        if (source != _normal) transition(source, Event::NormalMode, _normal);
        if (source != _screen) transition(source, Event::ScreenMode, _screen);
        if (source != panel) transition(source, Event::PanelMode, panel);
    }
    transition(idle, Event::Press, pressed);
    transition(pressed, Event::Release, idle);
    connect(transition(pressed, Event::CancelGesture, idle), &QAbstractTransition::triggered,
        this, &ModelWindowState::InteractionCancelled);
    transition(editIdle, Event::DragModel, drag);
    transition(editIdle, Event::MoveCrop, move);
    transition(editIdle, Event::ResizeCrop, resize);
    for (auto* source : {drag, move, resize}) {
        transition(source, Event::Release, editIdle);
        connect(transition(source, Event::CancelGesture, editIdle), &QAbstractTransition::triggered,
            this, &ModelWindowState::InteractionCancelled);
    }
    // Leaving a mode cancels its recognizers, including pending single/double
    // clicks. Parent exits also cover switching and closing during a gesture.
    for (auto* source : {_normal, _screen, panel})
        connect(source, &QState::exited, this, &ModelWindowState::InteractionCancelled);
    _machine.start();
    QCoreApplication::sendPostedEvents(&_machine, QEvent::MetaCall);
}

bool ModelWindowState::Send(Event event)
{
    const auto before = _machine.configuration();
    _machine.postEvent(new PolicyEvent(event));
    // Qt queues state-machine processing. Drain only this machine's work so
    // synchronous input/IPC handlers observe the committed state, without a
    // nested application event loop dispatching input or rendering reentrantly.
    QCoreApplication::sendPostedEvents(&_machine, QEvent::MetaCall);
    return before != _machine.configuration();
}

ModelWindowState::State ModelWindowState::Current() const
{
    const auto active = _machine.configuration();
    for (std::size_t i = 0; i < _states.size(); ++i)
        if (active.contains(_states[i])) return static_cast<State>(i);
    return State::Uninitialized;
}
bool ModelWindowState::Running() const { return _machine.configuration().contains(_running); }
bool ModelWindowState::Normal() const { return _machine.configuration().contains(_normal); }
bool ModelWindowState::ScreenEditing() const { return _machine.configuration().contains(_screen); }
bool ModelWindowState::GestureActive() const
{
    const auto state = Current();
    return state == State::Pressed || state == State::DraggingModel ||
        state == State::MovingCrop || state == State::ResizingCrop;
}
