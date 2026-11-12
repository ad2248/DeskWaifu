//! Geometry and layer-shell value types.

/// Anchor bit values as defined by `zwlr_layer_surface_v1.anchor`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Anchors(u32);

impl Anchors {
    /// Top edge.
    pub const TOP: Anchors = Anchors(1);
    /// Bottom edge.
    pub const BOTTOM: Anchors = Anchors(2);
    /// Left edge.
    pub const LEFT: Anchors = Anchors(4);
    /// Right edge.
    pub const RIGHT: Anchors = Anchors(8);

    /// No anchors at all (the compositor centres the surface).
    pub const NONE: Anchors = Anchors(0);

    /// Combine two anchor sets.
    #[must_use]
    pub const fn union(self, other: Anchors) -> Anchors {
        Anchors(self.0 | other.0)
    }

    /// The raw protocol bitfield.
    pub const fn bits(self) -> u32 {
        self.0
    }
}

impl std::ops::BitOr for Anchors {
    type Output = Anchors;

    fn bitor(self, rhs: Anchors) -> Anchors {
        self.union(rhs)
    }
}

/// Layer of the `zwlr_layer_shell_v1.layer` enum.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Layer {
    /// Background layer, behind normal windows.
    Background,
    /// Bottom layer.
    Bottom,
    /// Top layer.
    Top,
    /// Overlay layer, above normal windows and panels.
    Overlay,
}

/// Keyboard interactivity for the layer surface.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyboardInteractivity {
    /// Never receive keyboard focus. What the model window needs.
    None,
    /// Request exclusive keyboard focus.
    Exclusive,
}

/// A pixel rectangle in surface-local coordinates.
///
/// The caller maps its model input regions into these rectangles; this crate
/// knows nothing about model coordinates.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rect {
    /// Left edge.
    pub x: i32,
    /// Top edge.
    pub y: i32,
    /// Width in pixels, must be positive to be usable.
    pub width: i32,
    /// Height in pixels, must be positive to be usable.
    pub height: i32,
}

impl Rect {
    /// Create a rectangle.
    pub const fn new(x: i32, y: i32, width: i32, height: i32) -> Rect {
        Rect {
            x,
            y,
            width,
            height,
        }
    }

    /// Clip this rectangle to a `surface_width` x `surface_height` surface.
    ///
    /// Empty and negative sizes are rejected, as are rectangles that fall
    /// completely outside the surface.
    #[must_use]
    pub fn clip(self, surface_width: u32, surface_height: u32) -> Option<Rect> {
        if self.width <= 0 || self.height <= 0 {
            return None;
        }
        let surface_width = i64::from(surface_width);
        let surface_height = i64::from(surface_height);
        let left = i64::from(self.x);
        let top = i64::from(self.y);
        let right = left.saturating_add(i64::from(self.width));
        let bottom = top.saturating_add(i64::from(self.height));
        let left = left.max(0);
        let top = top.max(0);
        let right = right.min(surface_width);
        let bottom = bottom.min(surface_height);
        if right <= left || bottom <= top {
            return None;
        }
        Some(Rect {
            x: i32::try_from(left).ok()?,
            y: i32::try_from(top).ok()?,
            width: i32::try_from(right - left).ok()?,
            height: i32::try_from(bottom - top).ok()?,
        })
    }
}

impl From<(i32, i32, i32, i32)> for Rect {
    fn from((x, y, width, height): (i32, i32, i32, i32)) -> Rect {
        Rect::new(x, y, width, height)
    }
}

/// Resolve the surface size from the requested size and a `configure` size.
///
/// A zero configure dimension means "the client decides", so the requested
/// dimension is kept. A nonzero configure dimension is the compositor's answer
/// and wins.
#[must_use]
pub fn resolve_configured_size(requested: (u32, u32), configured: (u32, u32)) -> (u32, u32) {
    (
        if configured.0 == 0 {
            requested.0
        } else {
            configured.0
        },
        if configured.1 == 0 {
            requested.1
        } else {
            configured.1
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn anchors_compose_into_protocol_bits() {
        assert_eq!(Anchors::NONE.bits(), 0);
        assert_eq!((Anchors::TOP | Anchors::LEFT).bits(), 5);
        assert_eq!(
            (Anchors::TOP | Anchors::BOTTOM | Anchors::LEFT | Anchors::RIGHT).bits(),
            15
        );
    }

    #[test]
    fn rects_are_clipped_to_the_surface() {
        assert_eq!(
            Rect::new(10, 20, 30, 40).clip(800, 600),
            Some(Rect::new(10, 20, 30, 40))
        );
        assert_eq!(
            Rect::new(-10, -5, 30, 40).clip(800, 600),
            Some(Rect::new(0, 0, 20, 35))
        );
        assert_eq!(
            Rect::new(790, 590, 30, 40).clip(800, 600),
            Some(Rect::new(790, 590, 10, 10))
        );
        assert_eq!(Rect::new(-30, -40, 20, 30).clip(800, 600), None);
        assert_eq!(Rect::new(800, 0, 10, 10).clip(800, 600), None);
    }

    #[test]
    fn empty_and_negative_rects_are_rejected() {
        assert_eq!(Rect::new(0, 0, 0, 10).clip(800, 600), None);
        assert_eq!(Rect::new(0, 0, 10, 0).clip(800, 600), None);
        assert_eq!(Rect::new(0, 0, -5, 10).clip(800, 600), None);
        assert_eq!(Rect::new(0, 0, 10, -1).clip(800, 600), None);
        assert_eq!(Rect::new(0, 0, 10, 10).clip(0, 600), None);
    }

    #[test]
    fn configure_size_prefers_compositor_answers() {
        assert_eq!(resolve_configured_size((800, 600), (800, 600)), (800, 600));
        assert_eq!(resolve_configured_size((800, 600), (0, 0)), (800, 600));
        assert_eq!(resolve_configured_size((800, 600), (1024, 0)), (1024, 600));
        assert_eq!(resolve_configured_size((800, 600), (0, 480)), (800, 480));
        assert_eq!(resolve_configured_size((800, 600), (1, 1)), (1, 1));
    }

    #[test]
    fn missing_global_error_names_the_global() {
        let error = crate::Error::MissingGlobal("zwlr_layer_shell_v1");
        assert_eq!(
            error.to_string(),
            "missing required Wayland global: zwlr_layer_shell_v1"
        );
    }
}
