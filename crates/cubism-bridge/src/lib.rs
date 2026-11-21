//! Rust interface to the opt-in Cubism/EGL offscreen renderer.

#[cfg(feature = "native")]
#[allow(unsafe_code)]
mod native;

#[cfg(feature = "native")]
pub use native::{DeviceSelector, Error, Model, RenderDevice, Runtime, TapHit};

const MAX_DIMENSION: u32 = 8192;

/// Length in bytes of a tightly packed RGBA8 frame with supported dimensions.
pub fn rgba_len(width: u32, height: u32) -> Option<usize> {
    if !(1..=MAX_DIMENSION).contains(&width) || !(1..=MAX_DIMENSION).contains(&height) {
        return None;
    }
    (width as usize)
        .checked_mul(height as usize)?
        .checked_mul(4)
}

/// Convert premultiplied RGBA8 in-place to the straight alpha representation used by PNG.
pub fn unpremultiply_rgba(pixels: &mut [[u8; 4]]) {
    for [r, g, b, a] in pixels {
        let alpha = u32::from(*a);
        for channel in [r, g, b] {
            *channel = if alpha == 0 {
                0
            } else {
                ((u32::from(*channel) * 255 + alpha / 2) / alpha).min(255) as u8
            };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_dimensions_are_bounded() {
        // 用奇数宽高、零尺寸和超大尺寸检查 RGBA 长度与输入边界。
        assert_eq!(rgba_len(3, 7), Some(84));
        assert_eq!(rgba_len(0, 7), None);
        assert_eq!(rgba_len(8193, 1), None);
        assert_eq!(rgba_len(u32::MAX, u32::MAX), None);
    }

    #[test]
    fn png_alpha_is_straight() {
        // 用透明、半透明和不透明像素验证反预乘，保留 alpha 并清空全透明像素的 RGB。
        let mut pixels = [[9, 9, 9, 0], [64, 32, 128, 128], [10, 99, 222, 255]];
        unpremultiply_rgba(&mut pixels);
        assert_eq!(
            pixels,
            [[0, 0, 0, 0], [128, 64, 255, 128], [10, 99, 222, 255]]
        );
    }
}
