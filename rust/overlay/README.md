# overlay

A Wayland `wlr-layer-shell` overlay surface with an EGL window surface and
pointer input, for the DeskWaifu model window.

## Scope

This crate is a window/input-region/pointer primitive with EGL. It owns:

- one `wl_surface` with the `zwlr_layer_surface_v1` role (layer, anchors, size,
  exclusive zone, keyboard interactivity, optional margins, optional named
  output)
- the matching EGL display/config/context/window surface and its
  `wl_egl_window`, plus `make_current`, `swap_buffers` and `size`
- the input region: `Overlay::set_input_region` replaces it with a fresh
  `wl_region` of clipped rectangles and commits, mirroring the C++
  `DesktopWindow::SetInputRegion`. An empty slice is a `wl_region` with no
  boxes, i.e. the whole surface is click-through.
- pointer reporting: `Overlay::connect` binds one `wl_seat` over a defensive
  version range and asks it for a `wl_pointer` once the seat reports the
  pointer capability. All `wl_pointer` events for this surface arrive in
  `Overlay::take_events()`.

It deliberately does **not** include:

- animation, Cubism, or model code
- any coordinate mapping beyond pixel surface coordinates: callers translate
  model regions into `Rect` themselves
- pointer policy: no tap/double-click detection, no hit testing, no drag
  handling, no cursor surfaces, no gestures, no timers. That split is the same
  one the C++ side uses, where `InteractionProcessor` owns policy and
  `QtDesktopWindow` only reports input.
- an event-loop abstraction, settings, or a render loop

EGL is configured with `EGL_ALPHA_SIZE >= 8`, so the caller can clear with alpha
0 and get a fully transparent surface, then draw opaque pixels where the model
is. `Overlay::display()` and `Overlay::egl()` expose the EGL handles for
`eglGetProcAddress`-based GL loading.

Everything is `!Send`/`!Sync`. Each `Overlay` owns its `Connection`; a second
overlay needs a second connection.

## Pointer events

`Overlay::take_events()` drains the events collected by the last
`dispatch_pending`, `dispatch` or `roundtrip`, in compositor order. Each one
mirrors one C++ `IDesktopWindowEvents` callback:

| `PointerEvent`                  | C++ callback       | Source event                |
| ------------------------------- | ------------------ | --------------------------- |
| `Entered { x, y }`              | `PointerEntered`   | `wl_pointer.enter`          |
| `Left`                          | `PointerLeft`      | `wl_pointer.leave`          |
| `Moved { x, y }`                | `PointerMoved`     | `wl_pointer.motion`         |
| `Button { button, pressed }`    | `PointerButton`    | `wl_pointer.button`         |
| `Scroll { delta }`              | `PointerScroll`    | `wl_pointer.axis`/`axis_value120` |

- **Coordinate space**: surface-local pixels from the top-left corner, `x` right
  and `y` down - the same space as `Rect` and the input region. The protocol
  sends `wl_fixed_t`, which `wayland_client` converts with the standard `/ 256`
  (one unit is 1/256 pixel), so fractional pixel positions survive.
- **Buttons**: `0` left, `1` right, `2` middle, exactly the C++
  `QtDesktopWindow` mapping (`Qt::LeftButton`/`RightButton`/`MiddleButton`).
  Other buttons are dropped: the C++ maps everything else to `-1` and then skips
  `PointerButton` entirely, so no `-1` event is ever reported.
- **Scroll**: one mouse wheel notch is `1.0`, i.e. the C++ unit of Qt's eighths
  of a degree divided by `120`. `wl_pointer.axis_value120 / 120.0` is used when
  the frame carries it; otherwise the fixed `wl_pointer.axis` value is reported
  as is, in surface-local units (compositors before `wl_pointer` version 8 have
  no `value120`). The sign follows the Wayland axis, so positive means "content
  moves down"; Qt negates `value120` into `angleDelta().y()`, so the same
  physical notch is `-delta` in the C++ `PointerScroll`.
- **One event per frame**: a `wl_pointer.frame` closes the batch and every
  event is delivered exactly once. `axis` and its coupled `axis_value120` in one
  frame collapse into a single `Scroll`, and the scroll is appended after that
  frame's pointer events, as Qt flushes them.
- **No synthetic positions**: a button event carries only `button` and
  `pressed`. `QtDesktopWindow` calls `PointerMoved` again before each
  `PointerButton` because Qt hands the button its own position; here a position
  change is reported only by the `motion`/`enter` event that carried it, so a
  caller that needs the click position keeps the last `Moved`/`Entered`.
- **No pointer**: a compositor without `wl_seat`, or a seat that never reports
  the pointer capability, is not an error. The overlay still renders and still
  routes its input region; `take_events()` just stays empty.

`wl_pointer.axis_stop`, `axis_source`, `axis_discrete` (superseded by
`axis_value120`), `axis_relative_direction` and horizontal scrolling are
ignored, like the C++ reader of `QWheelEvent::angleDelta().y()`.

## Platform

Linux/Wayland only, and only on compositors that implement
`zwlr_layer_shell_v1` (KWin, Sway). Globals are bound over defensive version
ranges, so a compositor advertising a newer version than the shipped XML is
fine.

This crate is not yet wired into `deskwaifu-rust`, CMake, or the application.
The real desktop authorization and click-through acceptance still belongs to the
C++ smoke suite until the runtime adopts this crate.

## Running the live test

The real compositor proof is opt-in; the ordinary `cargo test` run skips it and
stays green:

```sh
DESKWAIFU_OVERLAY_LIVE_TEST=1 cargo test --test kwin -- --test-threads=1
```

With `DESKWAIFU_OVERLAY_LIVE_TEST=1` the test spawns a private
`kwin_wayland --virtual` instance with its own mode-0700 `XDG_RUNTIME_DIR`, and
requires `kwin_wayland` on `PATH`. It proves:

- the layer surface reaches a `configure` with a nonzero size, with
  `Top | Left` anchors, overlay layer, exclusive zone `-1` and no keyboard
  interactivity
- the EGL context and window surface work: a transparent clear reads back alpha
  0, an opaque clear reads back alpha 255, with no GL error
- real click routing: `org_kde_kwin_fake_input` injects pointer clicks, and a
  full-surface background layer counts the presses that reach it. An empty input
  region sends the click below, a shaped region around the centre sends it to
  the overlay instead, and clearing the region restores click-through.
- real pointer reporting: with a shaped region around the centre, an injected
  pointer move, a left press/release and a wheel notch at `(440, 330)` deliver
  `Entered`, `Moved`, `Button{0, true}`, `Button{0, false}` and one
  `Scroll` from `take_events()`, in that order, with every reported position
  inside the shaped rectangle (the assertion allows 1 px of compositor rounding,
  and the injected `pointer_motion_absolute` positions are compositor-global,
  which equal surface-local here because the surface is anchored `Top | Left` at
  the output origin and unscaled). KWin's fake device sends only the fixed
  `axis`, so the single `Scroll` is the injected `10.0` in surface-local units
  and not a `value120` detent count. Clearing the region reports `Left` and
  delivers no further overlay pointer events, with the click reaching the
  background surface instead.
- a second `Overlay` on its own connection also works

CI does not run this yet; the C++ smoke suite still carries the production
desktop acceptance.
