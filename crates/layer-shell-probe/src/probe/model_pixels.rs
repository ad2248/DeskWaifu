use super::{ModelFrame, Rect, Size, placement::Placement};
use std::ops::Range;

pub(super) type LogicalClip = (Range<u32>, Range<u32>);

pub(super) struct PaintedBounds {
    x: std::ops::Range<u32>,
    y: std::ops::Range<u32>,
}

pub(super) fn paint_placed(
    canvas: &mut [u8],
    Size(width, height): Size,
    Size(logical_width, logical_height): Size,
    frame: &ModelFrame<'_>,
    placement: Placement,
    clip: Option<&LogicalClip>,
) -> PaintedBounds {
    canvas.fill(0);
    let (model_width, model_height) = placement.size();
    let (left, top) = placement.origin;
    let (scale_x, scale_y) = (
        f64::from(width) / f64::from(logical_width),
        f64::from(height) / f64::from(logical_height),
    );
    let pixel_edge = |edge: f64, scale: f64, limit: u32| {
        (edge * scale - 0.5).ceil().clamp(0.0, f64::from(limit)) as u32
    };
    let ((clip_left, clip_right), (clip_top, clip_bottom)) = clip.map_or(
        (
            (0.0, f64::from(logical_width)),
            (0.0, f64::from(logical_height)),
        ),
        |(x, y)| {
            (
                (f64::from(x.start), f64::from(x.end)),
                (f64::from(y.start), f64::from(y.end)),
            )
        },
    );
    let x_start = pixel_edge(left.max(clip_left), scale_x, width);
    let x_end = pixel_edge((left + model_width).min(clip_right), scale_x, width);
    let y_start = pixel_edge(top.max(clip_top), scale_y, height);
    let y_end = pixel_edge((top + model_height).min(clip_bottom), scale_y, height);
    let Size(source_width, source_height) = frame.size;
    for y in y_start..y_end {
        let source_y = (((f64::from(y) + 0.5) / scale_y - top) / model_height
            * f64::from(source_height)) as u32;
        for x in x_start..x_end {
            let source_x = (((f64::from(x) + 0.5) / scale_x - left) / model_width
                * f64::from(source_width)) as u32;
            let source = ((source_y.min(source_height - 1) * source_width
                + source_x.min(source_width - 1))
                * 4) as usize;
            let target = ((y * width + x) * 4) as usize;
            write_pixel(canvas, target, frame, source);
        }
    }
    PaintedBounds {
        x: x_start..x_end,
        y: y_start..y_end,
    }
}

fn write_pixel(canvas: &mut [u8], target: usize, frame: &ModelFrame<'_>, source: usize) {
    let [r, g, b, a] = frame.pixels[source..source + 4]
        .try_into()
        .expect("RGBA pixel");
    canvas[target..target + 4].copy_from_slice(&u32::from_be_bytes([a, r, g, b]).to_ne_bytes());
}

pub(super) fn paint_model(canvas: &mut [u8], Size(width, height): Size, frame: &ModelFrame<'_>) {
    let Size(source_width, source_height) = frame.size;
    for y in 0..height {
        let source_y = ((y * 2 + 1) * source_height / (height * 2)) as usize;
        for x in 0..width {
            let source_x = ((x * 2 + 1) * source_width / (width * 2)) as usize;
            let source = (source_y * source_width as usize + source_x) * 4;
            let target = ((y * width + x) * 4) as usize;
            let [r, g, b, a] = frame.pixels[source..source + 4]
                .try_into()
                .expect("RGBA pixel");
            canvas[target..target + 4]
                .copy_from_slice(&u32::from_be_bytes([a, r, g, b]).to_ne_bytes());
        }
    }
}

pub(super) fn clip_region(region: &mut Vec<Rect>, x: &Range<u32>, y: &Range<u32>) {
    region.retain_mut(|rect| {
        let right = (rect.0 + rect.2).min(x.end as i32);
        let bottom = (rect.1 + rect.3).min(y.end as i32);
        let left = rect.0.max(x.start as i32);
        let top = rect.1.max(y.start as i32);
        if right <= left || bottom <= top {
            return false;
        }
        *rect = Rect(left, top, right - left, bottom - top);
        true
    });
}

/// Scale every premultiplied color channel and alpha by the same factor.
pub(super) fn apply_opacity(canvas: &mut [u8], opacity: f64) {
    let factor = (opacity * f64::from(u16::MAX)).round() as u32;
    match factor {
        0 => canvas.fill(0),
        65535 => {}
        _ => {
            for channel in canvas {
                *channel = ((u32::from(*channel) * factor + 32767) / 65535) as u8;
            }
        }
    }
}

pub(super) fn model_region(
    canvas: &[u8],
    Size(width, height): Size,
    Size(w, h): Size,
    bounds: Option<PaintedBounds>,
) -> Vec<Rect> {
    const BLOCK: u32 = 2;
    const HIT_ALPHA: u8 = 32;
    let logical_range = |range: std::ops::Range<u32>, buffer: u32, logical: u32| {
        let start = range.start * logical / buffer / BLOCK * BLOCK;
        let end = (range.end * logical).div_ceil(buffer);
        start..end
    };
    let (xs, ys) = if let Some(bounds) = bounds {
        if bounds.x.is_empty() || bounds.y.is_empty() {
            return Vec::new();
        }
        (
            logical_range(bounds.x, width, w),
            logical_range(bounds.y, height, h),
        )
    } else {
        (0..w, 0..h)
    };
    let x_edge = (xs.end.div_ceil(BLOCK) * BLOCK).min(w);
    let mut regions = Vec::new();
    for y in ys.step_by(BLOCK as usize) {
        let y_end = (y + BLOCK).min(h);
        let mut run = None;
        for x in xs.clone().step_by(BLOCK as usize) {
            let x_end = (x + BLOCK).min(w);
            let hit = (y * height / h..(y_end * height).div_ceil(h)).any(|py| {
                (x * width / w..(x_end * width).div_ceil(w)).any(|px| {
                    let offset = ((py * width + px) * 4) as usize;
                    let pixel = u32::from_ne_bytes(
                        canvas[offset..offset + 4].try_into().expect("ARGB pixel"),
                    );
                    (pixel >> 24) as u8 >= HIT_ALPHA
                })
            });
            if hit {
                run.get_or_insert(x);
            } else if let Some(start) = run.take() {
                regions.push(Rect(
                    start as i32,
                    y as i32,
                    (x - start) as i32,
                    (y_end - y) as i32,
                ));
            }
        }
        if let Some(start) = run {
            regions.push(Rect(
                start as i32,
                y as i32,
                (x_edge - start) as i32,
                (y_end - y) as i32,
            ));
        }
    }
    regions
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alpha_region_and_pixels_follow_the_same_scaled_frame() {
        // 用 1.5 倍缩放后的半透明边缘和透明像素验证通道转换及逻辑命中区。
        let mut rgba = vec![0; 6 * 3 * 4];
        rgba[4 * 4..4 * 4 + 4].copy_from_slice(&[16, 32, 64, 128]);
        let frame = ModelFrame {
            size: Size(6, 3),
            pixels: rgba,
            render: Box::new(|_, _| Ok(())),
            last_frame: None,
            animated: false,
            #[cfg(feature = "native")]
            active_expression_toggles: Vec::new(),
        };
        let mut argb = vec![255; 6 * 3 * 4];
        paint_model(&mut argb, Size(6, 3), &frame);
        assert_eq!(&argb[..4], &[0, 0, 0, 0]);
        assert_eq!(&argb[16..20], &0x8010_2040_u32.to_ne_bytes());
        assert!(matches!(
            model_region(&argb, Size(6, 3), Size(4, 2), None).as_slice(),
            [Rect(2, 0, 2, 2)]
        ));
    }
}

#[cfg(test)]
#[test]
fn restricting_alpha_scan_preserves_fractional_boundary_blocks() {
    // 在 1.5 倍缓冲区放置一个斜跨逻辑块边界的矩形，比较限域扫描和完整扫描。
    let logical = Size(12, 8);
    let buffer = Size(18, 12);
    let mut canvas = vec![0; 18 * 12 * 4];
    for y in 3..8 {
        for x in 4..11 {
            let offset = (y * 18 + x) * 4;
            canvas[offset..offset + 4].copy_from_slice(&0xff00_0000_u32.to_ne_bytes());
        }
    }
    let bounds = PaintedBounds { x: 4..11, y: 3..8 };
    let rects = |region: Vec<Rect>| {
        region
            .into_iter()
            .map(|Rect(x, y, w, h)| (x, y, w, h))
            .collect::<Vec<_>>()
    };
    assert_eq!(
        rects(model_region(&canvas, buffer, logical, None)),
        rects(model_region(&canvas, buffer, logical, Some(bounds)))
    );
}

#[cfg(test)]
#[test]
fn crop_clips_pixels_and_hit_region_at_the_same_logical_edge() {
    // 以 1.5 倍缩放画出不透明内容，验证裁剪边界外的像素与逻辑命中同时消失。
    let frame = ModelFrame {
        size: Size(1, 1),
        pixels: vec![64, 128, 192, 255],
        render: Box::new(|_, _| Ok(())),
        last_frame: None,
        animated: false,
        #[cfg(feature = "native")]
        active_expression_toggles: Vec::new(),
    };
    let logical = Size(8, 6);
    let pixels = Size(12, 9);
    let mut canvas = vec![0; 12 * 9 * 4];
    let clip = (3..5, 1..4);
    let bounds = paint_placed(
        &mut canvas,
        pixels,
        logical,
        &frame,
        Placement::default(),
        Some(&clip),
    );
    assert_eq!(&canvas[..4], &[0; 4]);
    assert_eq!(
        &canvas[(2 * 12 + 5) * 4..(2 * 12 + 6) * 4],
        &0xff40_80c0_u32.to_ne_bytes()
    );
    let mut region = model_region(&canvas, pixels, logical, Some(bounds));
    clip_region(&mut region, &clip.0, &clip.1);
    assert_eq!(
        region
            .iter()
            .map(|&Rect(x, y, w, h)| (x, y, w, h))
            .collect::<Vec<_>>(),
        [(3, 1, 2, 1), (3, 2, 2, 2)]
    );
    apply_opacity(&mut canvas, 0.0);
    assert!(canvas.iter().all(|&byte| byte == 0));
}
