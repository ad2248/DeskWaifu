# Model window state machine

`ModelWindowState` uses Qt State Machine (`QStateMachine`, `QState`, and
`QAbstractTransition`) to control lifecycle and input policy. The active Qt state
configuration is the source of truth; there is no parallel lifecycle enum or
end-of-application flag.

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
  gesture data, not another state. Release returns to `EditIdle`; existing
  outside clicks and crop-frame double clicks still explicitly apply the draft.
- `PanelEditing` represents a control-panel-only transaction. It rejects model
  mouse, wheel, and keyboard actions, while allowing IPC transaction updates.
- `TransformEditor` owns draft/commit/cancel and connection ownership. Its
  synchronous edit-change callback changes input mode immediately, including
  disconnect, apply, cancel, reset, and reload/discard. It does not depend on Qt.
- Leaving a mode clears gesture data, pending taps, and keyboard capture.
  Resize and hide also cancel any captured gesture. Hiding does not save or
  cancel an edit transaction.

## Output changes and teardown

The window backend reports `OutputSwitchStarted` before its nested expose wait,
and `OutputSwitchFinished` after success or rollback. During replacement the
policy rejects input. The backend's `_switching` guard remains local to surface
replacement, preventing nested rendering or a second surface replacement.

An intentional switch cancels the gesture, not the draft. This preserves the
existing local cross-monitor move and explicit-save behavior. A hotplug switch
also cancels the edit transaction. After replacement the mode is restored from
the editor, without resuming a captured gesture. A close received during the
wait cannot be undone by the completion notification.

Initialization errors and application exit enter `Closing`. Existing ordered GPU
cleanup enters `Closed` only on success; a failed context restoration leaves the
policy in `Closing` and reports failure as before.

## Dispatch and tests

Calls run on the application's Qt thread, with a `QCoreApplication` present.
`Send` posts a policy event and drains only the state machine's posted meta-call
events. This commits transitions before synchronous input/IPC handlers continue,
without pumping unrelated application events. Cancellation callbacks only reset
gesture/platform data; they must not recursively dispatch policy events.

`model-window-state` tests the real Qt state graph, invalid events, cancellation,
failure/close transitions, and absence of unrelated event-loop dispatch.
`model-pointer-state` exercises the production Delegate, editor, and model input
with an isolated EGL window boundary, including crop movement/resizing, panel
transactions, stale releases, switch-time input, hotplug, and close during a
switch. Other pointer, keyboard, editing, and teardown tests remain applicable.
