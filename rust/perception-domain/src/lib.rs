//! Pure validation policies for perception image dimensions and screenshot inputs.

pub mod ctc;
pub mod line_order;

/// Maximum width or height accepted when creating a PNG from an image.
pub const MAX_PNG_SIDE: i128 = 4096;
/// Maximum screenshot file size in bytes (64 MiB).
pub const MAX_SCREENSHOT_FILE_BYTES: u64 = 64 * 1024 * 1024;
/// Maximum width or height accepted for a screenshot.
pub const MAX_SCREENSHOT_SIDE: i128 = 16_384;
/// Maximum number of screenshot pixels.
pub const MAX_SCREENSHOT_PIXELS: i128 = 32 * 1024 * 1024;

/// Rejection reasons for the image-to-PNG dimension policy.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PngImageError {
    NullOrZeroDimension,
    SideTooLarge,
}

/// Rejection reasons for screenshot input limits.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ScreenshotInputError {
    FileTooLarge,
    InvalidDimensions,
    SideTooLarge,
    PixelCountOverflow,
    TooManyPixels,
}

/// Validate dimensions before encoding an image as PNG.
///
/// This policy intentionally imposes no total pixel-count limit.
pub fn validate_image_to_png(width: i128, height: i128) -> Result<(), PngImageError> {
    if width <= 0 || height <= 0 {
        return Err(PngImageError::NullOrZeroDimension);
    }
    if width > MAX_PNG_SIDE || height > MAX_PNG_SIDE {
        return Err(PngImageError::SideTooLarge);
    }
    Ok(())
}

/// Multiply positive dimensions without allowing integer wraparound.
///
/// Exposed so callers and tests can use the same overflow-safe calculation.
pub fn checked_pixel_count(width: i128, height: i128) -> Result<i128, ScreenshotInputError> {
    width
        .checked_mul(height)
        .ok_or(ScreenshotInputError::PixelCountOverflow)
}

/// Validate screenshot file size and decoded image dimensions.
///
/// The size limit is inclusive. Dimensions use `i128` so negative values can
/// be rejected and multiplication overflow can be reported explicitly.
pub fn validate_screenshot_input(
    file_size_bytes: u64,
    width: i128,
    height: i128,
) -> Result<(), ScreenshotInputError> {
    if file_size_bytes > MAX_SCREENSHOT_FILE_BYTES {
        return Err(ScreenshotInputError::FileTooLarge);
    }
    if width <= 0 || height <= 0 {
        return Err(ScreenshotInputError::InvalidDimensions);
    }
    if width > MAX_SCREENSHOT_SIDE || height > MAX_SCREENSHOT_SIDE {
        return Err(ScreenshotInputError::SideTooLarge);
    }
    let pixels = checked_pixel_count(width, height)?;
    if pixels > MAX_SCREENSHOT_PIXELS {
        return Err(ScreenshotInputError::TooManyPixels);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn png_policy_rejects_zero_and_nonpositive_dimensions() {
        for (width, height) in [(0, 1), (1, 0), (-1, 1), (1, -1)] {
            assert_eq!(
                validate_image_to_png(width, height),
                Err(PngImageError::NullOrZeroDimension)
            );
        }
    }

    #[test]
    fn png_policy_accepts_exact_side_limit_without_pixel_cap() {
        assert_eq!(validate_image_to_png(4096, 4096), Ok(()));
        assert_eq!(validate_image_to_png(4096, 4095), Ok(()));
    }

    #[test]
    fn png_policy_rejects_side_just_over_limit() {
        assert_eq!(
            validate_image_to_png(4097, 1),
            Err(PngImageError::SideTooLarge)
        );
        assert_eq!(
            validate_image_to_png(1, 4097),
            Err(PngImageError::SideTooLarge)
        );
    }

    #[test]
    fn screenshot_policy_accepts_file_size_limit_and_pixel_limit() {
        assert_eq!(
            validate_screenshot_input(MAX_SCREENSHOT_FILE_BYTES, 8192, 4096),
            Ok(())
        );
        assert_eq!(
            validate_screenshot_input(MAX_SCREENSHOT_FILE_BYTES, 1, 1),
            Ok(())
        );
    }

    #[test]
    fn screenshot_policy_rejects_file_size_just_over_limit() {
        assert_eq!(
            validate_screenshot_input(MAX_SCREENSHOT_FILE_BYTES + 1, 1, 1),
            Err(ScreenshotInputError::FileTooLarge)
        );
    }

    #[test]
    fn screenshot_policy_rejects_invalid_dimensions() {
        for (width, height) in [(0, 1), (1, 0), (-1, 1), (1, -1)] {
            assert_eq!(
                validate_screenshot_input(0, width, height),
                Err(ScreenshotInputError::InvalidDimensions)
            );
        }
    }

    #[test]
    fn screenshot_policy_rejects_side_just_over_limit() {
        assert_eq!(
            validate_screenshot_input(0, 16_385, 1),
            Err(ScreenshotInputError::SideTooLarge)
        );
        assert_eq!(
            validate_screenshot_input(0, 1, 16_385),
            Err(ScreenshotInputError::SideTooLarge)
        );
    }

    #[test]
    fn screenshot_policy_rejects_pixel_count_just_over_cap() {
        assert_eq!(
            validate_screenshot_input(0, 8193, 4096),
            Err(ScreenshotInputError::TooManyPixels)
        );
    }

    #[test]
    fn screenshot_pixel_count_reports_multiplication_overflow() {
        assert_eq!(
            checked_pixel_count(i128::MAX, i128::MAX),
            Err(ScreenshotInputError::PixelCountOverflow)
        );
    }
}
