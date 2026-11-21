use super::{OVERLAY_SIZE, Size};

/// Requested crop in logical output coordinates. The requested rectangle may extend off screen.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[cfg_attr(feature = "native", derive(serde::Serialize))]
#[cfg_attr(not(feature = "native"), allow(dead_code))]
pub struct Crop {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
}

#[cfg_attr(not(feature = "native"), allow(dead_code))]
impl Crop {
    pub fn new(x: i32, y: i32, width: u32, height: u32) -> Option<Self> {
        (width > 0 && height > 0).then_some(Self {
            x,
            y,
            width,
            height,
        })
    }

    #[cfg(feature = "native")]
    pub(super) fn parts(self) -> (i32, i32, u32, u32) {
        (self.x, self.y, self.width, self.height)
    }

    pub(super) fn clip(self, viewport: Size) -> (std::ops::Range<u32>, std::ops::Range<u32>) {
        let axis = |start: i32, length: u32, limit: u32| {
            let start = i64::from(start);
            let limit = i64::from(limit);
            start.clamp(0, limit) as u32..(start + i64::from(length)).clamp(0, limit) as u32
        };
        (
            axis(self.x, self.width, viewport.0),
            axis(self.y, self.height, viewport.1),
        )
    }
}

#[cfg(feature = "native")]
#[derive(Clone, Copy, Debug, PartialEq, serde::Serialize)]
pub struct Transform {
    x: f64,
    y: f64,
    zoom: f64,
    pub crop: Option<Crop>,
}

#[cfg(feature = "native")]
impl Transform {
    pub fn new(x: f64, y: f64, zoom: f64, crop: Option<Crop>) -> Option<Self> {
        (x.is_finite() && y.is_finite() && (0.1..=10.0).contains(&zoom)).then_some(Self {
            x,
            y,
            zoom,
            crop,
        })
    }

    pub(super) fn placement(self) -> Placement {
        Placement {
            origin: (self.x, self.y),
            zoom: self.zoom,
        }
    }

    pub(super) fn capture(placement: Placement, crop: Option<Crop>) -> Self {
        Self {
            x: placement.origin.0,
            y: placement.origin.1,
            zoom: placement.zoom,
            crop,
        }
    }
}

#[cfg(feature = "native")]
impl Default for Transform {
    fn default() -> Self {
        Self::capture(Placement::default(), None)
    }
}

#[derive(Clone, Copy)]
pub(super) struct Placement {
    pub origin: (f64, f64),
    pub zoom: f64,
}

impl Default for Placement {
    fn default() -> Self {
        Self {
            origin: (0.0, 0.0),
            zoom: 1.0,
        }
    }
}

impl Placement {
    pub fn size(self) -> (f64, f64) {
        (
            f64::from(OVERLAY_SIZE.0) * self.zoom,
            f64::from(OVERLAY_SIZE.1) * self.zoom,
        )
    }

    #[cfg(any(feature = "native", test))]
    pub fn constrain(&mut self, viewport: Size) {
        let (width, height) = self.size();
        // Keep the center reachable after moving, even if the canvas is larger than the output.
        self.origin.0 =
            (self.origin.0 + width / 2.0).clamp(0.0, f64::from(viewport.0)) - width / 2.0;
        self.origin.1 =
            (self.origin.1 + height / 2.0).clamp(0.0, f64::from(viewport.1)) - height / 2.0;
    }

    pub fn look_at(self, target: (f64, f64), radius: f64) -> (f32, f32) {
        let (width, height) = self.size();
        let x = ((target.0 - self.origin.0) / width - 0.5) * 2.0;
        let y = (0.5 - (target.1 - self.origin.1) / height) * 2.0;
        let length = x.hypot(y);
        let scale = if length > radius.min(1.0) {
            radius.min(1.0) / length
        } else {
            1.0
        };
        ((x * scale) as f32, (y * scale) as f32)
    }

    #[cfg(any(feature = "native", test))]
    pub fn scroll(&mut self, cursor: (f64, f64), steps: f64, viewport: Size) {
        let zoom = (self.zoom * 1.12_f64.powf(steps.clamp(-8.0, 8.0))).clamp(0.1, 10.0);
        let ratio = zoom / self.zoom;
        self.origin.0 = cursor.0 - (cursor.0 - self.origin.0) * ratio;
        self.origin.1 = cursor.1 - (cursor.1 - self.origin.1) * ratio;
        self.zoom = zoom;
        self.constrain(viewport);
    }
}

#[cfg(any(feature = "native", test))]
pub(super) fn drag_threshold(delta: (f64, f64)) -> bool {
    delta.0.hypot(delta.1) >= 4.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gaze_keeps_direction_when_limited() {
        // 使用右上方鼠标坐标，验证方向、半径限制和半径零时的居中行为。
        let placement = Placement::default();
        let (x, y) = placement.look_at((320.0, 0.0), 0.25);
        assert!((x.hypot(y) - 0.25).abs() < 1e-6);
        assert_eq!(x, y);
        assert_eq!(placement.look_at((320.0, 0.0), 0.0), (0.0, 0.0));
    }

    #[test]
    fn drag_threshold_and_zoom_keep_pointer_coordinates_stable() {
        // 先尝试小幅移动，再跨过阈值，最后检查光标锚点放大后仍落在原位置。
        let viewport = Size(1600, 900);
        let mut placement = Placement::default();
        assert!(!drag_threshold((2.0, 1.0)));
        assert!(drag_threshold((4.0, 0.0)));
        placement.origin = (80.0, 80.0);
        placement.scroll((230.0, 180.0), 1.0, viewport);
        assert!((placement.zoom - 1.12).abs() < 1e-9);
        assert!((placement.origin.0 + 150.0 * placement.zoom - 230.0).abs() < 1e-9);
        assert!((placement.origin.1 + 100.0 * placement.zoom - 180.0).abs() < 1e-9);
    }
}
