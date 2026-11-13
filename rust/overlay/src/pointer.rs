//! Pointer event reporting, the input half of the overlay surface.
//!
//! This is the Rust counterpart of the `IDesktopWindowEvents` pointer callbacks
//! that the C++ `QtDesktopWindow` invokes: [`PointerEvent`] names exactly the
//! information of `PointerEntered`, `PointerLeft`, `PointerMoved`,
//! `PointerButton` and `PointerScroll`, and nothing more. Tap/double-click
//! detection, hit testing and drag policy stay with the caller, just as they
//! stay in the C++ `InteractionProcessor` rather than in the window.
//!
//! Events are collected here in compositor order and handed to the caller by
//! [`crate::Overlay::take_events`].

use wayland_client::protocol::wl_pointer;
use wayland_client::WEnum;

/// Whether an axis is the vertical scroll axis; unknown reserved values are not.
fn is_vertical(axis: WEnum<wl_pointer::Axis>) -> bool {
    matches!(axis, WEnum::Value(wl_pointer::Axis::VerticalScroll))
}

/// One pointer input event, already in surface-local coordinates.
///
/// Coordinates are pixels from the top-left corner of the overlay surface, the
/// same space as [`crate::Rect`] and the input region. `x` grows right, `y`
/// grows down.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PointerEvent {
    /// The pointer entered the surface at `(x, y)`.
    Entered {
        /// Surface-local x coordinate.
        x: f64,
        /// Surface-local y coordinate.
        y: f64,
    },
    /// The pointer left the surface.
    Left,
    /// The pointer moved to `(x, y)`.
    Moved {
        /// Surface-local x coordinate.
        x: f64,
        /// Surface-local y coordinate.
        y: f64,
    },
    /// A mapped pointer button changed state.
    ///
    /// `button` is the application numbering used by the C++
    /// `IDesktopWindowEvents::PointerButton`: `0` left, `1` right, `2` middle.
    /// Buttons outside that mapping are not reported at all, matching
    /// `QtDesktopWindow`, which only calls `PointerButton` for `>= 0`.
    Button {
        /// `0` left, `1` right, `2` middle.
        button: i32,
        /// `true` for press, `false` for release.
        pressed: bool,
    },
    /// A vertical wheel scroll of `delta` wheel detents.
    ///
    /// One mouse wheel notch is `1.0`, i.e. the unit of the C++ value, which
    /// divides Qt's eighths of a degree by `120`. The sign follows the
    /// `wl_pointer` axis, so positive is "content moves down"; Qt negates
    /// `axis_value120` into `angleDelta().y()`, which makes the C++
    /// `PointerScroll` argument `-delta` for the same physical notch.
    Scroll {
        /// Scroll distance in wheel detents, positive for the Wayland down
        /// direction.
        delta: f64,
    },
}

/// The C++ button mapping: kernel `BTN_LEFT`/`BTN_RIGHT`/`BTN_MIDDLE` to
/// `0`/`1`/`2`, everything else unmapped.
///
/// `QtDesktopWindow` maps `Qt::LeftButton -> 0`, `Qt::RightButton -> 1`,
/// `Qt::MiddleButton -> 2` and every other Qt button to `-1`, then skips
/// `PointerButton` entirely for `-1`. Qt climbs from the same kernel codes, so
/// the three mapped evdev codes are exactly the C++ reachable set.
fn button_number(button: u32) -> Option<i32> {
    match button {
        0x110 => Some(0),
        0x111 => Some(1),
        0x112 => Some(2),
        _ => None,
    }
}

/// Accumulates the events of one `wl_pointer` frame and flushes them once.
///
/// A frame is the unit the compositor documents as "logically belonging
/// together"; flushing on the frame boundary is what keeps a single physical
/// wheel notch from becoming two scroll events, because version 8 pointers send
/// both `axis` and `axis_value120` for it.
#[derive(Debug, Default)]
pub(crate) struct PointerFrame {
    events: Vec<PointerEvent>,
    /// Vertical scroll from `wl_pointer.axis_value120`, in wheel detents.
    detents: f64,
    /// Vertical scroll from `wl_pointer.axis`, in surface-local units, used
    /// only when the frame carried no `value120`.
    axis: f64,
    /// Whether `value120` was present, including an explicit zero.
    has_value120: bool,
}

impl PointerFrame {
    /// Record that the pointer entered at `(x, y)`.
    pub(crate) fn entered(&mut self, x: f64, y: f64) {
        self.events.push(PointerEvent::Entered { x, y });
    }

    /// Record that the pointer left.
    pub(crate) fn left(&mut self) {
        self.events.push(PointerEvent::Left);
    }

    /// Record a motion to `(x, y)`.
    pub(crate) fn moved(&mut self, x: f64, y: f64) {
        self.events.push(PointerEvent::Moved { x, y });
    }

    /// Record a button change, dropping unmapped buttons.
    pub(crate) fn button(&mut self, button: u32, pressed: bool) {
        if let Some(button) = button_number(button) {
            self.events.push(PointerEvent::Button { button, pressed });
        }
    }

    /// Record a `wl_pointer.axis` value; only the vertical axis is reported,
    /// matching the C++ reader of `QWheelEvent::angleDelta().y()`.
    pub(crate) fn axis(&mut self, axis: WEnum<wl_pointer::Axis>, value: f64) {
        if is_vertical(axis) {
            self.axis += value;
        }
    }

    /// Record a `wl_pointer.axis_value120` value; only vertical is reported.
    pub(crate) fn axis_value120(&mut self, axis: WEnum<wl_pointer::Axis>, value120: i32) {
        if is_vertical(axis) {
            self.has_value120 = true;
            self.detents += f64::from(value120) / 120.0;
        }
    }

    /// Append the frame's events to `out`, at most one `Scroll` at the end,
    /// then start a new frame. The scroll comes last because Qt flushes the
    /// pointer event before the wheel event of the same frame.
    pub(crate) fn flush(&mut self, out: &mut Vec<PointerEvent>) {
        out.append(&mut self.events);
        let delta = if self.has_value120 {
            self.detents
        } else {
            // `axis` is already a surface-local double (wl_fixed converted by
            // wayland-client); normalize its 10-unit legacy notch to 120.
            self.axis / 10.0
        };
        self.detents = 0.0;
        self.axis = 0.0;
        self.has_value120 = false;
        if delta != 0.0 {
            out.push(PointerEvent::Scroll { delta });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vertical() -> WEnum<wl_pointer::Axis> {
        WEnum::Value(wl_pointer::Axis::VerticalScroll)
    }

    fn horizontal() -> WEnum<wl_pointer::Axis> {
        WEnum::Value(wl_pointer::Axis::HorizontalScroll)
    }

    fn flushed(frame: &mut PointerFrame) -> Vec<PointerEvent> {
        let mut out = Vec::new();
        frame.flush(&mut out);
        out
    }

    #[test]
    fn an_unknown_axis_is_ignored() {
        let mut frame = PointerFrame::default();
        frame.axis(WEnum::Unknown(7), 3.0);
        assert_eq!(flushed(&mut frame), Vec::new());
    }

    #[test]
    fn buttons_map_like_the_cpp_window() {
        assert_eq!(button_number(0x110), Some(0)); // BTN_LEFT
        assert_eq!(button_number(0x111), Some(1)); // BTN_RIGHT
        assert_eq!(button_number(0x112), Some(2)); // BTN_MIDDLE

        // Side buttons reach Qt as ExtraButton1, which the C++ maps to -1.
        assert_eq!(button_number(0x113), None);
        assert_eq!(button_number(0x120), None);
        assert_eq!(button_number(0), None);
    }

    #[test]
    fn unmapped_buttons_are_not_reported() {
        let mut frame = PointerFrame::default();
        frame.button(0x113, true);
        assert_eq!(flushed(&mut frame), Vec::new());
    }

    #[test]
    fn a_frame_reports_each_event_once_in_order() {
        let mut frame = PointerFrame::default();
        frame.entered(10.5, 20.25);
        frame.moved(11.0, 21.0);
        frame.button(0x110, true);
        frame.button(0x110, false);
        frame.left();
        assert_eq!(
            flushed(&mut frame),
            vec![
                PointerEvent::Entered { x: 10.5, y: 20.25 },
                PointerEvent::Moved { x: 11.0, y: 21.0 },
                PointerEvent::Button {
                    button: 0,
                    pressed: true
                },
                PointerEvent::Button {
                    button: 0,
                    pressed: false
                },
                PointerEvent::Left,
            ]
        );
        // The next frame starts empty: nothing is replayed.
        assert_eq!(flushed(&mut frame), Vec::new());
    }

    #[test]
    fn a_value120_notch_becomes_one_scroll() {
        let mut frame = PointerFrame::default();
        frame.axis_value120(vertical(), 120);
        frame.axis_value120(vertical(), -120);
        frame.axis_value120(vertical(), 60);
        assert_eq!(
            flushed(&mut frame),
            vec![PointerEvent::Scroll { delta: 0.5 }]
        );
    }

    #[test]
    fn value120_wins_over_the_coupled_fixed_axis() {
        // A version 8 compositor sends both for one notch; reporting both would
        // double the scroll.
        let mut frame = PointerFrame::default();
        frame.axis_value120(vertical(), 120);
        frame.axis(vertical(), 15.0);
        assert_eq!(
            flushed(&mut frame),
            vec![PointerEvent::Scroll { delta: 1.0 }]
        );
    }

    #[test]
    fn the_fixed_axis_fallback_converts_legacy_ten_unit_notches() {
        let mut frame = PointerFrame::default();
        frame.axis(vertical(), 7.5);
        frame.axis(vertical(), 2.5);
        assert_eq!(
            flushed(&mut frame),
            vec![PointerEvent::Scroll { delta: 1.0 }]
        );
    }

    #[test]
    fn horizontal_axes_are_ignored() {
        // The C++ reads only QWheelEvent::angleDelta().y().
        let mut frame = PointerFrame::default();
        frame.axis(horizontal(), 3.0);
        frame.axis_value120(horizontal(), 120);
        assert_eq!(flushed(&mut frame), Vec::new());
    }
}
