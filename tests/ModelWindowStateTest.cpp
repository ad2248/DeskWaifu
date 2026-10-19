#include "ModelWindowState.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Line " << __LINE__ << ": " << #condition << '\n'; return 1; \
} } while (false)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using State = ModelWindowState::State;
    using Event = ModelWindowState::Event;
    ModelWindowState policy;
    int cancelled = 0;
    QObject::connect(&policy, &ModelWindowState::InteractionCancelled, [&] { ++cancelled; });
    bool unrelatedEvent = false;
    QTimer::singleShot(0, &app, [&] { unrelatedEvent = true; });
    CHECK(policy.Current() == State::Uninitialized);
    CHECK(!policy.Send(Event::Press) && !policy.Running());
    CHECK(policy.Send(Event::Initialize) && policy.Current() == State::Initializing);
    CHECK(!policy.Send(Event::DragModel));
    CHECK(policy.Send(Event::Initialized) && policy.Normal());
    CHECK(policy.Current() == State::NormalIdle);
    CHECK(policy.Send(Event::Press) && policy.Current() == State::Pressed);
    CHECK(!policy.Send(Event::Press) && policy.GestureActive());
    CHECK(policy.Send(Event::Release) && !policy.GestureActive());
    CHECK(policy.Send(Event::Press));
    CHECK(policy.Send(Event::ScreenMode) && policy.Current() == State::EditIdle);
    CHECK(cancelled == 1);
    for (const auto event : {Event::DragModel, Event::MoveCrop, Event::ResizeCrop}) {
        const auto expected = event == Event::DragModel ? State::DraggingModel :
            event == Event::MoveCrop ? State::MovingCrop : State::ResizingCrop;
        CHECK(policy.Send(event) && policy.Current() == expected);
        CHECK(policy.ScreenEditing() && policy.Running() && policy.GestureActive());
        CHECK(!policy.Send(Event::Press));
        CHECK(policy.Send(Event::Release) && policy.Current() == State::EditIdle);
        CHECK(policy.Send(event));
        CHECK(policy.Send(Event::CancelGesture) && !policy.GestureActive());
    }
    CHECK(cancelled == 4);
    CHECK(policy.Send(Event::PanelMode) && policy.Current() == State::PanelEditing);
    CHECK(!policy.Send(Event::Press) && !policy.Normal() && !policy.ScreenEditing());
    CHECK(policy.Send(Event::NormalMode));
    CHECK(policy.Send(Event::Press));
    CHECK(policy.Send(Event::SwitchOutput) && policy.Current() == State::SwitchingOutput);
    CHECK(!policy.Running() && !policy.GestureActive());
    CHECK(!policy.Send(Event::Press) && !policy.Send(Event::ScreenMode));
    CHECK(policy.Send(Event::OutputReady) && policy.Current() == State::NormalIdle);
    CHECK(policy.Send(Event::ScreenMode));
    CHECK(policy.Send(Event::ResizeCrop));
    CHECK(policy.Send(Event::Close) && policy.Current() == State::Closing);
    CHECK(!policy.Send(Event::Press) && !policy.Send(Event::Initialized));
    CHECK(!policy.Send(Event::OutputReady));
    CHECK(policy.Send(Event::Released) && policy.Current() == State::Closed);
    CHECK(!policy.Send(Event::Initialize) && !policy.Send(Event::Close));
    CHECK(!unrelatedEvent); // Dispatch must never pump the application's event loop.
    for (const auto event : {Event::Initialize, Event::Initialized, Event::SwitchOutput}) {
        ModelWindowState failure;
        failure.Send(Event::Initialize);
        if (event != Event::Initialize) failure.Send(Event::Initialized);
        if (event == Event::SwitchOutput) failure.Send(Event::SwitchOutput);
        CHECK(failure.Send(Event::Close));
        CHECK(!failure.Send(Event::OutputReady));
        CHECK(failure.Send(Event::Released) && failure.Current() == State::Closed);
    }
    return 0;
}
