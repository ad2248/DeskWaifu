# Model window state machine

`src/plugins/desktop/private/processor/interaction/ModelWindowState` uses Qt State Machine (`QStateMachine`, `QState`, and
`QAbstractTransition`) to control lifecycle and input policy. The active Qt state
configuration is the source of truth for window policy, with no parallel window
lifecycle enum. `InteractionProcessor` also has a `_stopped` flag that gates its
input and operation handling independently of the state machine.

```text
Uninitialized -> Initializing -> Running
                                |-- Normal
                                |   |-- NormalIdle
                                |   `-- Pressed
                                |-- ScreenEditing
                                |   |-- EditIdle
                                |   |-- DraggingModel
                                |   |-- MovingCrop
                                |   `-- ResizingCrop
                                `-- PanelEditing
Running -> SwitchingOutput -> Running
Uninitialized / Initializing / Running / SwitchingOutput -> Closing -> Closed
```

## Input and editing

- Normal press/release retains the existing delayed tap and double-click
  recognizer. Moving beyond its threshold starts a local screen-edit transaction
  and enters `ScreenEditing/DraggingModel`, as before this refactor. There is no
  separate ordinary dragging state because dragging already opens the editor.
- In screen editing, hit testing selects model movement, movement of the model
  and crop together, or crop-edge/corner resizing. The selected edge/corner is
  gesture data, not another state. Release normally returns to `EditIdle`;
  outside clicks and crop-frame double clicks attempt to apply the draft. A
  successful apply ends editing; a failed apply retains the transaction.
- `PanelEditing` represents a control-panel-only transaction. It rejects model
  mouse-button, wheel, and keyboard actions, while allowing IPC transaction
  updates. Pointer position and hover tracking are not blocked by this mode.
- `TransformEditor` owns draft/commit/cancel and connection ownership. Its
  synchronous edit-change callback synchronizes input mode while `Running`,
  including disconnect and successful apply, cancel, reset, and reload/discard.
  During output replacement, mode synchronization waits for completion.
  `TransformEditor` does not depend on Qt.
- Leaving a mode clears gesture data, pending taps, and keyboard capture.
  Frame-size changes and successful hiding also cancel any captured gesture.
  Frame-size changes additionally cancel screen-edit transactions; hiding does
  not save or cancel an edit transaction.

## Output changes and teardown

The window backend reports `OutputSwitchStarted` before its nested expose wait,
and `OutputSwitchFinished` after success or rollback. During replacement the
policy rejects input. The backend's `_switching` guard remains local to surface
replacement, preventing nested rendering or a second surface replacement.

A local cross-monitor move cancels the gesture, not the draft, preserving
explicit-save behavior. Explicit display selection through the control command
is rejected while editing. A hotplug rebind cancels any edit transaction,
including panel-only editing. After replacement the mode is restored from the
editor, without resuming a captured gesture. A close received during the wait
cannot be undone by the completion notification.

Application exit enters `Closing`. Initialization failures return to the caller
without necessarily entering `Closing`: window startup failures can leave the
policy in `Initializing`, and later application startup failures can leave it in
`Running`. The caller's release path stops interaction and enters `Closing`.
Ordered teardown stops the model worker before shutting down the window.
Window shutdown then sends `Released` and enters `Closed`, even if model GPU
cleanup failed. Release still reports that failure; `Closed` does not certify
successful GPU cleanup or context restoration.

## Dispatch and tests

Calls run on the application's Qt thread, with a `QCoreApplication` present.
`Send` posts a policy event and drains only the state machine's posted meta-call
events. This commits transitions before synchronous input/IPC handlers continue,
without pumping unrelated application events. Cancellation callbacks only reset
gesture/platform data; they must not recursively dispatch policy events.

`model-window-state` tests the real Qt state graph, invalid events, cancellation,
explicit close/release transitions from startup, running, and switching states,
and absence of unrelated event-loop dispatch. It does not inject actual startup
or GPU cleanup failures.
`model-pointer-state` exercises the production Delegate, editor, and model input
with an isolated EGL window boundary, including crop movement/resizing, panel
transactions, stale releases, switch-time input, hotplug, and close during a
switch. This boundary simulates output replacement rather than exercising the
Qt backend's nested expose wait and rollback. `model-shutdown-context` checks
model-worker cleanup failure and avoidance of unsafe GL deletion, not the
application window's final policy state. Other pointer, keyboard, editing, and
teardown tests remain applicable.
