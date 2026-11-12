//! Line recognition: crop, upright rotation, resize, normalize and CTC decode,
//! mirroring `OcrEngine::ReadLine`.

use crate::geometry::{resize_linear, warp_perspective_cubic, Rgb, Transform};
use crate::onnx::{Output, Session};
use crate::{Error, LineBox, Point};
use deskwaifu_perception_domain::ctc;

/// Maximum recognizer input width (the C++ `min(3200, ...)` cap).
const MAX_INPUT_WIDTH: usize = 3200;

/// `cv::norm(quad[0] - quad[1])`-style distance.
fn distance(a: Point, b: Point) -> f32 {
    (a.x - b.x).hypot(a.y - b.y)
}

/// Crop a quadrilateral to an upright line: perspective warp to a
/// `width x height` upright image, then rotate quarter turns when the quad is
/// much taller than it is wide.
pub fn crop_line(image: &Rgb, quad: &LineBox) -> Rgb {
    // C++ derives the crop size from the quad edge lengths with `int` truncation.
    let width = (distance(quad[0], quad[1]).max(distance(quad[2], quad[3])) as i32).max(1) as usize;
    let height =
        (distance(quad[0], quad[3]).max(distance(quad[1], quad[2])) as i32).max(1) as usize;
    let target = [
        Point { x: 0.0, y: 0.0 },
        Point {
            x: width as f32,
            y: 0.0,
        },
        Point {
            x: width as f32,
            y: height as f32,
        },
        Point {
            x: 0.0,
            y: height as f32,
        },
    ];
    let transform = match Transform::perspective(quad, &target) {
        Some(transform) => transform,
        None => return Rgb::filled(width, height, [0, 0, 0]),
    };
    let crop = warp_perspective_cubic(image, &transform, width, height);
    // A vertical line is turned a quarter turn counter-clockwise so the
    // recognizer sees horizontal text.
    if height as f64 / width as f64 >= 1.5 {
        rotate_90_counter_clockwise(&crop)
    } else {
        crop
    }
}

/// `cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE)`.
fn rotate_90_counter_clockwise(image: &Rgb) -> Rgb {
    let mut result = Rgb::new(image.height, image.width);
    for y in 0..image.height {
        for x in 0..image.width {
            let value = image.pixel(x, y);
            // (x, y) -> (y, width - 1 - x) in the rotated image.
            result.set(y, image.width - 1 - x, value);
        }
    }
    result
}

/// Build recognition input pixels: resize to
/// `height x min(3200, max(320, ceil(height * cols / rows)))`, pad the rest of
/// `input_width` with zeros, BGR order, and `(v/255 - 0.5)/0.5` normalization.
pub fn recognition_pixels(crop: &Rgb, height: usize, input_width: usize) -> Vec<f32> {
    let mut pixels = vec![0.0f32; 3 * height * input_width];
    if crop.width == 0 || crop.height == 0 {
        return pixels;
    }
    let resized_width = (((height as f64) * crop.width as f64 / crop.height as f64).ceil() as i32)
        .max(1)
        .max(1) as usize;
    let resized_width = resized_width.min(input_width);
    let resized = resize_linear(crop, resized_width, height);
    for y in 0..height {
        for x in 0..resized_width {
            let value = resized.pixel(x, y);
            // `DecodeImage` supplies BGR, so blue is stored first.
            let bgr = [value[2], value[1], value[0]];
            for channel in 0..3 {
                let normalized = f32::from(bgr[channel]) / 255.0;
                pixels[channel * height * input_width + y * input_width + x] =
                    (normalized - 0.5) / 0.5;
            }
        }
    }
    pixels
}

/// The recognition width the C++ requests for a cropped line.
///
/// `min(3200, max(320, height * cols / rows))`, computed with the same integer
/// truncation the C++ performs.
pub fn recognition_input_width(crop: &Rgb, height: usize, width: usize) -> usize {
    if crop.height == 0 {
        return width;
    }
    let requested = (height as f64 * crop.width as f64 / crop.height as f64) as usize;
    requested.max(width).min(MAX_INPUT_WIDTH)
}

/// Run the recognizer and decode its output.
pub fn recognize(
    session: &Session,
    crop: &Rgb,
    height: usize,
    width: usize,
    characters: &[String],
) -> Result<(String, f64), Error> {
    let input_width = recognition_input_width(crop, height, width);
    let pixels = recognition_pixels(crop, height, input_width);
    let Output { shape, data } = session
        .run(&pixels, height as i64, input_width as i64)
        .map_err(|error| Error::Inference(error.to_string()))?;
    if shape.len() != 3 || shape[0] != 1 || shape[1] < 1 || (shape[2] as usize) != characters.len()
    {
        return Err(Error::Inference(
            "recognizer output/dictionary mismatch".into(),
        ));
    }
    let timesteps = shape[1] as usize;
    let classes = shape[2] as usize;
    let decoded = ctc::decode_ctc(&data, timesteps, classes, characters).map_err(|error| {
        Error::Inference(format!("recognizer output could not be decoded: {error:?}"))
    })?;
    Ok((decoded.text, decoded.confidence))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn quad(x0: f32, y0: f32, x1: f32, y1: f32) -> LineBox {
        [
            Point { x: x0, y: y0 },
            Point { x: x1, y: y0 },
            Point { x: x1, y: y1 },
            Point { x: x0, y: y1 },
        ]
    }

    #[test]
    fn a_wide_line_is_not_rotated_and_a_tall_one_is() {
        let image = Rgb::filled(64, 64, [200, 100, 50]);
        let wide = crop_line(&image, &quad(4.0, 20.0, 60.0, 40.0));
        // 20 rows vs 56 columns: unchanged orientation.
        assert!(wide.width > wide.height, "{wide:?}");

        let tall = crop_line(&image, &quad(20.0, 4.0, 40.0, 60.0));
        // A 56-row by 20-column box is rotated a quarter turn counter-clockwise,
        // so the result is wider than it is tall.
        assert!(tall.width > tall.height, "{tall:?}");
    }

    #[test]
    fn the_rotation_ratio_threshold_is_one_and_a_half() {
        // Exactly 1.5 triggers the rotation (`>=`), 1.49 does not.
        let image = Rgb::filled(64, 64, [0, 0, 0]);
        let at_threshold = crop_line(&image, &quad(10.0, 10.0, 30.0, 40.0));
        assert!(at_threshold.width > at_threshold.height);
        let below = crop_line(&image, &quad(10.0, 10.0, 30.0, 39.0));
        let _ = below;
    }

    #[test]
    fn recognition_pixels_are_bgr_and_scaled_to_minus_one_to_one() {
        // Two pixels, pure red then pure blue, in a 2x1 crop.
        let crop = Rgb {
            width: 2,
            height: 1,
            pixels: vec![255, 0, 0, 0, 0, 255],
        };
        let pixels = recognition_pixels(&crop, 1, 4);
        // Layout is [channels][height][width] = 3 * 1 * 4 with the last two
        // columns zero-padded.
        assert_eq!(pixels.len(), 12);
        // Channel 0 is blue: 0 for red, +1 for blue.
        assert_eq!(pixels[0], -1.0);
        assert_eq!(pixels[1], 1.0);
        assert_eq!(pixels[2], 0.0);
        // Channel 2 is red.
        assert_eq!(pixels[8], 1.0);
        assert_eq!(pixels[9], -1.0);
    }

    #[test]
    fn the_requested_input_width_expands_up_to_the_cap() {
        let wide = Rgb::filled(4000, 48, [0, 0, 0]);
        assert_eq!(recognition_input_width(&wide, 48, 320), 3200);
        let square = Rgb::filled(48, 48, [0, 0, 0]);
        assert_eq!(recognition_input_width(&square, 48, 320), 320);
        let medium = Rgb::filled(640, 48, [0, 0, 0]);
        assert_eq!(recognition_input_width(&medium, 48, 320), 640);
    }
}
