//! DB text detection: preprocessing, probability-map thresholding and box
//! extraction, mirroring `OcrEngine::Detect`.

use crate::contours::find_contours;
use crate::geometry::{fill_polygon, min_area_rect, resize_linear, Rgb, RotatedRect};
use crate::metadata::PostProcess;
use crate::onnx::{Output, Session};
use crate::Point;

/// The detector input size chosen for an image: shortest side at least 64,
/// longest side at most 4000, both rounded to a multiple of 32.
pub fn detector_input_size(width: usize, height: usize) -> (usize, usize) {
    let mut ratio = if width.min(height) < 64 {
        64.0 / width.min(height) as f64
    } else {
        1.0
    };
    let mut scaled_width = (width as f64 * ratio) as i32;
    let mut scaled_height = (height as f64 * ratio) as i32;
    if scaled_width.max(scaled_height) > 4000 {
        ratio = 4000.0 / scaled_width.max(scaled_height) as f64;
        scaled_height = (scaled_height as f64 * ratio) as i32;
        scaled_width = (scaled_width as f64 * ratio) as i32;
    }
    // `std::nearbyint` rounds half to even, then the result is clamped to 32.
    let round_to_32 = |value: i32| -> usize {
        let rounded = (f64::from(value) / 32.0).round_ties_even() as i32 * 32;
        rounded.max(32) as usize
    };
    (round_to_32(scaled_width), round_to_32(scaled_height))
}

/// Build the detector input tensor: BGR bands, `/255`, per-channel
/// normalization, HWC to CHW, padded to `padded_width`.
pub fn detector_pixels(
    image: &Rgb,
    padded_width: usize,
    mean: [f32; 3],
    stddev: [f32; 3],
) -> Vec<f32> {
    let rows = image.height;
    let mut pixels = vec![0.0f32; 3 * rows * padded_width];
    for y in 0..rows {
        for x in 0..image.width {
            let offset = (y * image.width + x) * 3;
            // `DecodeImage` is BGR, so channel 0 receives the blue value.
            let bgr = [
                image.pixels[offset + 2],
                image.pixels[offset + 1],
                image.pixels[offset],
            ];
            for channel in 0..3 {
                let value = f32::from(bgr[channel]) / 255.0;
                pixels[channel * rows * padded_width + y * padded_width + x] =
                    (value - mean[channel]) / stddev[channel];
            }
        }
    }
    pixels
}

/// `Corners(rect)`: sort the four box points by x, order each pair by y, then
/// emit top-left, top-right, bottom-right, bottom-left.
pub fn corners(rect: &RotatedRect) -> [Point; 4] {
    let mut points = rect.points();
    points.sort_by(|a, b| a.x.total_cmp(&b.x));
    if points[0].y > points[1].y {
        points.swap(0, 1);
    }
    if points[2].y > points[3].y {
        points.swap(2, 3);
    }
    [points[0], points[2], points[3], points[1]]
}

fn distance(a: Point, b: Point) -> f32 {
    (a.x - b.x).hypot(a.y - b.y)
}

/// Mean of `probability` over the pixels the mask selects.
fn masked_mean(probability: &[f32], width: usize, mask: &[u8], mask_width: usize) -> f32 {
    let mask_height = mask.len() / mask_width.max(1);
    let mut total = 0.0f64;
    let mut count = 0usize;
    for y in 0..mask_height {
        for x in 0..mask_width {
            if mask[y * mask_width + x] != 0 {
                total += f64::from(probability[y * width + x]);
                count += 1;
            }
        }
    }
    // `cv::mean` divides by the selected count, falling back to 0.
    if count == 0 {
        0.0
    } else {
        (total / count as f64) as f32
    }
}

/// Turn a detector probability map into image-space quadrilaterals.
pub fn boxes(
    output: &Output,
    source_width: usize,
    source_height: usize,
    postprocess: PostProcess,
) -> Result<Vec<[Point; 4]>, crate::Error> {
    validate_output_shape(output)?;
    let rows = output.shape[2] as usize;
    let columns = output.shape[3] as usize;
    let probability = &output.data;

    // `cv::Mat bitmap = probability > thresh` under a non-inverted comparison.
    let mut bitmap = vec![0u8; rows * columns];
    for (index, value) in probability.iter().enumerate() {
        if *value > postprocess.thresh {
            bitmap[index] = 1;
        }
    }

    let contours = find_contours(&bitmap, columns, rows);
    let mut found = Vec::new();
    for contour in contours.iter().take(postprocess.max_candidates) {
        let points: Vec<(i32, i32)> = contour.clone();
        let mut rect = min_area_rect(&points);
        if rect.width.min(rect.height) < 3.0 {
            continue;
        }
        let quad = corners(&rect);
        let (mut x_min, mut x_max) = (quad[0].x, quad[0].x);
        let (mut y_min, mut y_max) = (quad[0].y, quad[0].y);
        for point in quad {
            x_min = x_min.min(point.x);
            x_max = x_max.max(point.x);
            y_min = y_min.min(point.y);
            y_max = y_max.max(point.y);
        }
        let left = 0.max(x_min.floor() as i32);
        let top = 0.max(y_min.floor() as i32);
        let right = (columns as i32 - 1).min(x_max.ceil() as i32);
        let bottom = (rows as i32 - 1).min(y_max.ceil() as i32);
        if right < left || bottom < top {
            continue;
        }
        let mask_width = (right - left + 1) as usize;
        let mask_height = (bottom - top + 1) as usize;
        let local: Vec<(i32, i32)> = quad
            .iter()
            .map(|point| (point.x as i32 - left, point.y as i32 - top))
            .collect();
        let mask = fill_polygon(mask_width, mask_height, &local);
        // The C++ slices `probability` to the box and computes the mean there.
        let mut cropped = vec![0.0f32; mask_width * mask_height];
        for y in 0..mask_height {
            let source = (top as usize + y) * columns + left as usize;
            cropped[y * mask_width..(y + 1) * mask_width]
                .copy_from_slice(&probability[source..source + mask_width]);
        }
        if masked_mean(&cropped, mask_width, &mask, mask_width) < postprocess.box_thresh {
            continue;
        }

        // DB quad unclip: expand the minimum-area rectangle by
        // `2 * area * ratio / perimeter` per side, in floating point.
        let expansion = rect.width * rect.height * postprocess.unclip_ratio
            / (2.0 * (rect.width + rect.height));
        rect.width += 2.0 * expansion;
        rect.height += 2.0 * expansion;
        if rect.width.min(rect.height) < 5.0 {
            continue;
        }
        let mut unscaled = corners(&rect);
        for point in &mut unscaled {
            point.x = 0.0f32.max((source_width as f32 - 1.0).min(
                (f64::from(point.x) * source_width as f64 / columns as f64).round_ties_even()
                    as f32,
            ));
            point.y = 0.0f32.max((source_height as f32 - 1.0).min(
                (f64::from(point.y) * source_height as f64 / rows as f64).round_ties_even() as f32,
            ));
        }
        if distance(unscaled[0], unscaled[1]) <= 3.0 || distance(unscaled[0], unscaled[3]) <= 3.0 {
            continue;
        }
        found.push(unscaled);
    }

    found.sort_by(|a, b| {
        if a[0].y == b[0].y {
            a[0].x.total_cmp(&b[0].x)
        } else {
            a[0].y.total_cmp(&b[0].y)
        }
    });
    // Paddle's local row sort, shared with the domain crate.
    deskwaifu_perception_domain::line_order::sort_line_boxes(&mut found);
    Ok(found)
}

/// Resize the image to the detector input size and run the model.
pub fn detect(
    session: &Session,
    image: &Rgb,
    mean: [f32; 3],
    stddev: [f32; 3],
) -> Result<Output, crate::Error> {
    let (width, height) = detector_input_size(image.width, image.height);
    let resized = resize_linear(image, width, height);
    let pixels = detector_pixels(&resized, width, mean, stddev);
    let output = session
        .run(&pixels, height as i64, width as i64)
        .map_err(|error| crate::Error::Inference(error.to_string()))?;
    // `OcrEngine::Detect` validates the runtime output shape immediately after
    // the session call, before any postprocessing, so oversized dynamic outputs
    // fail during construction rather than mid-image.
    validate_output_shape(&output)?;
    Ok(output)
}

/// The C++ runtime shape check: `[1, 1, rows, columns]` with `1..=4096` sides.
pub fn validate_output_shape(output: &Output) -> Result<(), crate::Error> {
    if output.shape.len() != 4
        || output.shape[0] != 1
        || output.shape[1] != 1
        || output.shape[2] < 1
        || output.shape[3] < 1
        || output.shape[2] > 4096
        || output.shape[3] > 4096
    {
        return Err(crate::Error::Inference(
            "unexpected detector output shape".into(),
        ));
    }
    let rows = output.shape[2] as usize;
    let columns = output.shape[3] as usize;
    if output.data.len() != rows * columns {
        return Err(crate::Error::Inference(
            "detector output element count does not match its shape".into(),
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn input_size_clamps_then_rounds_to_multiples_of_32() {
        // Reference values from the C++ `Detect` arithmetic, checked against a
        // build of the same expressions: shortest side 64, longest side 4000.
        for (width, height, expected) in [
            (64usize, 64usize, (64usize, 64usize)),
            (100, 37, (160, 64)),
            (4096, 4096, (4000, 4000)),
            (10, 10, (64, 64)),
            (3000, 20, (4000, 32)),
            (5000, 100, (4000, 64)),
        ] {
            assert_eq!(
                detector_input_size(width, height),
                expected,
                "{width}x{height}"
            );
        }
    }

    #[test]
    fn input_size_never_goes_below_32() {
        for (width, height) in [(1usize, 1usize), (1, 200), (200, 1), (2, 2)] {
            let (width, height) = detector_input_size(width, height);
            assert!(width >= 32 && height >= 32, "{width}x{height}");
            assert_eq!(width % 32, 0);
            assert_eq!(height % 32, 0);
        }
    }

    #[test]
    fn pixels_are_bgr_normalized_and_channel_planar() {
        // One pixel: R=255, G=0, B=128.
        let image = Rgb {
            width: 1,
            height: 1,
            pixels: vec![255, 0, 128],
        };
        let pixels = detector_pixels(&image, 33, [0.485, 0.456, 0.406], [0.229, 0.224, 0.225]);
        assert_eq!(pixels.len(), 3 * 33);
        // Channel 0 is blue (128), channel 1 green (0), channel 2 red (255).
        assert!((pixels[0] - ((128.0 / 255.0 - 0.485) / 0.229)).abs() < 1e-6);
        assert!((pixels[33] - ((0.0 - 0.456) / 0.224)).abs() < 1e-6);
        assert!((pixels[66] - ((255.0 / 255.0 - 0.406) / 0.225)).abs() < 1e-6);
        // The padding to width 33 stays zero.
        assert_eq!(pixels[1], 0.0);
        assert_eq!(pixels[34], 0.0);
    }

    #[test]
    fn corners_order_top_left_top_right_bottom_right_bottom_left() {
        let rect = RotatedRect {
            center: Point { x: 10.0, y: 10.0 },
            width: 8.0,
            height: 4.0,
            angle: 0.0,
        };
        let corners = corners(&rect);
        let expected = [(6.0, 8.0), (14.0, 8.0), (14.0, 12.0), (6.0, 12.0)];
        for (point, (x, y)) in corners.iter().zip(expected.iter()) {
            assert!((point.x - x).abs() < 1e-4, "{corners:?}");
            assert!((point.y - y).abs() < 1e-4, "{corners:?}");
        }
    }

    #[test]
    fn the_masked_mean_uses_only_selected_pixels() {
        let probability = [0.0f32, 1.0, 1.0, 0.0];
        let mask = [0u8, 1, 1, 0];
        assert_eq!(masked_mean(&probability, 2, &mask, 2), 1.0);
        // An empty mask yields zero rather than a division error.
        assert_eq!(masked_mean(&probability, 2, &[0, 0, 0, 0], 2), 0.0);
    }

    #[test]
    fn an_output_shape_with_too_many_sides_is_rejected() {
        let output = Output {
            shape: vec![1, 1, 4097, 1],
            data: vec![0.0; 4097],
        };
        assert!(validate_output_shape(&output).is_err());
        let output = Output {
            shape: vec![1, 1, 8, 8],
            data: vec![0.0; 8 * 8],
        };
        assert!(validate_output_shape(&output).is_ok());
    }
}
