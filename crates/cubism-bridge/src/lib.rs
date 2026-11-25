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
            *channel = (u32::from(*channel) * 255 + alpha / 2)
                .checked_div(alpha)
                .unwrap_or(0)
                .min(255) as u8;
        }
    }
}
