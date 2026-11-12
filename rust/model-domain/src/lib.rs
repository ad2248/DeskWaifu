//! Pure model geometry, transforms, pointer-input regions, and model catalog discovery.

pub mod catalog;

/// Surface-local rectangle with a top-left origin.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Region {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

impl Region {
    pub const fn new(x: i32, y: i32, width: i32, height: i32) -> Self {
        Self {
            x,
            y,
            width,
            height,
        }
    }

    /// Returns whether the point is inside, including left/top but excluding right/bottom.
    pub fn contains(self, x: f64, y: f64) -> bool {
        x >= f64::from(self.x)
            && y >= f64::from(self.y)
            && x < f64::from(self.x) + f64::from(self.width)
            && y < f64::from(self.y) + f64::from(self.height)
    }
}

/// Intersection of two regions, or the empty region when either has no area.
pub fn intersect(a: Region, b: Region) -> Region {
    let x = a.x.max(b.x);
    let y = a.y.max(b.y);
    let right = (i64::from(a.x) + i64::from(a.width)).min(i64::from(b.x) + i64::from(b.width));
    let bottom = (i64::from(a.y) + i64::from(a.height)).min(i64::from(b.y) + i64::from(b.height));
    if a.width <= 0
        || a.height <= 0
        || b.width <= 0
        || b.height <= 0
        || right <= i64::from(x)
        || bottom <= i64::from(y)
    {
        Region::default()
    } else {
        Region::new(
            x,
            y,
            (right - i64::from(x)) as i32,
            (bottom - i64::from(y)) as i32,
        )
    }
}

/// Uses the full nonnegative surface when unconfigured; otherwise clips the request.
pub fn resolve(configured: bool, requested: Region, width: i32, height: i32) -> Region {
    let surface = Region::new(0, 0, width.max(0), height.max(0));
    if configured {
        intersect(requested, surface)
    } else {
        surface
    }
}

/// Converts a top-left-origin region to OpenGL's bottom-left-origin coordinates without clipping.
pub fn to_opengl(region: Region, surface_height: i32) -> Region {
    Region::new(
        region.x,
        surface_height - region.y - region.height,
        region.width,
        region.height,
    )
}

/// Returns outside pieces in top, bottom, left, right order. Zero-area pieces are retained.
pub fn outside(region: Region, width: i32, height: i32) -> [Region; 4] {
    [
        Region::new(0, 0, width, region.y),
        Region::new(
            0,
            region.y + region.height,
            width,
            height - region.y - region.height,
        ),
        Region::new(0, region.y, region.x, region.height),
        Region::new(
            region.x + region.width,
            region.y,
            width - region.x - region.width,
            region.height,
        ),
    ]
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Transform {
    pub scale: f32,
    pub x: f32,
    pub y: f32,
}

impl Transform {
    pub const fn new(scale: f32, x: f32, y: f32) -> Self {
        Self { scale, x, y }
    }

    /// Applies the projection's scale-then-translation composition.
    pub fn transform_point(self, point_x: f32, point_y: f32) -> (f32, f32) {
        (
            self.scale * (point_x + self.x),
            self.scale * (point_y + self.y),
        )
    }

    /// Zooms around a logical-coordinate anchor, clamping scale to [0.1, 10].
    pub fn zoom_around(self, anchor_x: f32, anchor_y: f32, requested_scale: f32) -> Self {
        let scale = requested_scale.clamp(0.1, 10.0);
        let mut result = Self { scale, ..self };
        if self.scale > 0.0 && scale != self.scale {
            let delta = 1.0 / scale - 1.0 / self.scale;
            result.x += anchor_x * delta;
            result.y += anchor_y * delta;
        }
        result
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PixelBufferTooShort {
    pub required: usize,
    pub actual: usize,
}

impl std::fmt::Display for PixelBufferTooShort {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "pixel buffer is too short: need {} bytes, got {}",
            self.required, self.actual
        )
    }
}

impl std::error::Error for PixelBufferTooShort {}

/// Extracts opaque sampled blocks from bottom-up RGBA8 data into clipped top-left-origin runs.
pub fn extract_input_regions(
    pixels: &[u8],
    width: i32,
    height: i32,
    clip: Region,
) -> Result<Vec<Region>, PixelBufferTooShort> {
    if width <= 0 || height <= 0 {
        return Ok(Vec::new());
    }
    let required = (width as usize)
        .checked_mul(height as usize)
        .and_then(|n| n.checked_mul(4))
        .expect("positive i32 surface dimensions fit usize");
    if pixels.len() < required {
        return Err(PixelBufferTooShort {
            required,
            actual: pixels.len(),
        });
    }
    let clip = intersect(clip, Region::new(0, 0, width, height));
    let mut regions = Vec::new();
    let mut y = 0;
    while y < height {
        let h = 10.min(height - y);
        let mut start: Option<i32> = None;
        let mut x = 0;
        while x < width {
            let w = 10.min(width - x);
            let sample = (((y + h / 2) as usize * width as usize) + (x + w / 2) as usize) * 4 + 3;
            if pixels[sample] > 10 {
                start.get_or_insert(x);
            } else if let Some(begin) = start.take() {
                append_clipped(&mut regions, begin, x, y, h, height, clip);
            }
            x += w;
        }
        if let Some(begin) = start {
            append_clipped(&mut regions, begin, width, y, h, height, clip);
        }
        y += h;
    }
    Ok(regions)
}

fn append_clipped(
    regions: &mut Vec<Region>,
    start: i32,
    end: i32,
    y: i32,
    h: i32,
    height: i32,
    clip: Region,
) {
    let box_region = Region::new(start, height - y - h, end - start, h);
    let clipped = intersect(box_region, clip);
    if clipped.width > 0 && clipped.height > 0 {
        regions.push(clipped);
    }
}

/// Returns whether a point lies in any input region.
pub fn input_region_contains(regions: &[Region], x: f64, y: f64) -> bool {
    regions.iter().copied().any(|region| region.contains(x, y))
}

/// In screen-edit mode the full surface receives input, regardless of model regions.
pub fn frame_input_regions(
    screen_editing: bool,
    width: i32,
    height: i32,
    model_regions: &[Region],
) -> Vec<Region> {
    if screen_editing {
        vec![Region::new(0, 0, width, height)]
    } else {
        model_regions.to_vec()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn region_intersection_resolution_and_edges() {
        assert_eq!(
            intersect(Region::new(-2, 1, 5, 4), Region::new(0, 0, 3, 3)),
            Region::new(0, 1, 3, 2)
        );
        assert_eq!(
            intersect(Region::new(0, 0, 0, 2), Region::new(0, 0, 2, 2)),
            Region::default()
        );
        assert_eq!(
            intersect(Region::new(0, 0, 2, 2), Region::new(4, 4, 2, 2)),
            Region::default()
        );
        assert_eq!(
            resolve(false, Region::new(8, 8, 1, 1), -1, 3),
            Region::new(0, 0, 0, 3)
        );
        assert_eq!(
            resolve(true, Region::new(-1, 2, 5, 5), 3, 4),
            Region::new(0, 2, 3, 2)
        );
        assert_eq!(
            to_opengl(Region::new(2, 3, 4, 5), 20),
            Region::new(2, 12, 4, 5)
        );
        assert!(Region::new(2, 3, 4, 5).contains(2.0, 3.0));
        assert!(!Region::new(2, 3, 4, 5).contains(6.0, 4.0));
        assert!(!Region::new(2, 3, 4, 5).contains(3.0, 8.0));
    }

    #[test]
    fn outside_keeps_order_and_zero_sized_pieces() {
        assert_eq!(
            outside(Region::new(2, 3, 4, 5), 10, 12),
            [
                Region::new(0, 0, 10, 3),
                Region::new(0, 8, 10, 4),
                Region::new(0, 3, 2, 5),
                Region::new(6, 3, 4, 5),
            ]
        );
        assert_eq!(
            outside(Region::new(0, 0, 10, 12), 10, 12),
            [
                Region::new(0, 0, 10, 0),
                Region::new(0, 12, 10, 0),
                Region::new(0, 0, 0, 12),
                Region::new(10, 0, 0, 12),
            ]
        );
    }

    #[test]
    fn transform_and_zoom_match_projection_contract() {
        let t = Transform::new(2.0, 0.3, -0.2);
        let transformed = t.transform_point(0.4, -0.1);
        assert!((transformed.0 - 1.4).abs() < 1e-6 && (transformed.1 + 0.6).abs() < 1e-6);
        let before = Transform::new(2.0, 0.2, -0.1);
        let after = before.zoom_around(-1.2, -0.7, 1.8);
        let point_x = -1.2 / before.scale - before.x;
        let point_y = -0.7 / before.scale - before.y;
        let projected = after.transform_point(point_x, point_y);
        assert!((projected.0 + 1.2).abs() < 1e-5 && (projected.1 + 0.7).abs() < 1e-5);
        assert_eq!(
            Transform::new(0.1, 0.2, -0.1).zoom_around(1.0, -1.0, 0.09),
            Transform::new(0.1, 0.2, -0.1)
        );
        assert_eq!(
            Transform::new(10.0, 0.2, -0.1).zoom_around(1.0, -1.0, 11.0),
            Transform::new(10.0, 0.2, -0.1)
        );
        assert_eq!(
            Transform::new(0.105, 0.2, -0.1)
                .zoom_around(0.8, -0.4, 0.09)
                .scale,
            0.1
        );
        assert_eq!(
            Transform::new(0.0, 0.3, -0.2).zoom_around(0.9, -0.6, 2.0),
            Transform::new(2.0, 0.3, -0.2)
        );
    }

    #[test]
    fn input_regions_sample_blocks_convert_y_and_clip() {
        let (width, height) = (27, 27);
        let pixels = vec![255; width * height * 4];
        assert_eq!(
            extract_input_regions(
                &pixels,
                width as i32,
                height as i32,
                Region::new(0, 0, 27, 27)
            )
            .unwrap(),
            [
                Region::new(0, 17, 27, 10),
                Region::new(0, 7, 27, 10),
                Region::new(0, 0, 27, 7),
            ]
        );
        let mut pixels = vec![0; width * height * 4];
        pixels[(5 * width + 5) * 4 + 3] = 11;
        pixels[(5 * width + 15) * 4 + 3] = 10;
        pixels[(5 * width + 23) * 4 + 3] = 255;
        pixels[(15 * width + 5) * 4 + 3] = 255;
        pixels[(15 * width + 15) * 4 + 3] = 255;
        assert_eq!(
            extract_input_regions(&pixels, 27, 27, Region::new(0, 0, 27, 27)).unwrap(),
            [
                Region::new(0, 17, 10, 10),
                Region::new(20, 17, 7, 10),
                Region::new(0, 7, 20, 10),
            ]
        );
        let short = vec![255; 23 * 24 * 4];
        assert_eq!(
            extract_input_regions(&short, 23, 24, Region::new(0, 0, 23, 24)).unwrap(),
            [
                Region::new(0, 14, 23, 10),
                Region::new(0, 4, 23, 10),
                Region::new(0, 0, 23, 4),
            ]
        );
        let clipped =
            extract_input_regions(&vec![255; 27 * 27 * 4], 27, 27, Region::new(3, 2, 5, 4))
                .unwrap();
        assert_eq!(clipped, [Region::new(3, 2, 5, 4)]);
    }

    #[test]
    fn opaque_input_regions_cover_each_clipped_pixel_exactly_once() {
        let pixels = vec![255; 27 * 27 * 4];
        for requested in [
            Region::new(3, 2, 5, 4),
            Region::new(9, 6, 2, 13),
            Region::new(-3, -2, 8, 6),
            Region::new(25, 25, 8, 9),
            Region::new(27, 0, 4, 9),
            Region::new(0, 27, 9, 4),
            Region::new(-8, 0, 4, 9),
            Region::new(0, -8, 9, 4),
            Region::default(),
        ] {
            let clip = resolve(true, requested, 27, 27);
            let regions = extract_input_regions(&pixels, 27, 27, requested).unwrap();
            for y in 0..27 {
                for x in 0..27 {
                    let count = regions
                        .iter()
                        .filter(|r| r.contains(x as f64, y as f64))
                        .count();
                    assert_eq!(count, usize::from(clip.contains(x as f64, y as f64)));
                }
            }
        }
    }

    #[test]
    fn zoom_round_trip_restores_translation() {
        let before = Transform::new(1.0, 0.3, -0.2);
        let restored = before
            .zoom_around(0.9, -0.6, 2.0)
            .zoom_around(0.9, -0.6, 1.0);
        assert_eq!(restored.scale, before.scale);
        assert!((restored.x - before.x).abs() < 0.00001);
        assert!((restored.y - before.y).abs() < 0.00001);
    }

    #[test]
    fn input_regions_reject_short_buffers_and_frame_region_overrides_model() {
        assert_eq!(
            extract_input_regions(&[], 1, 1, Region::new(0, 0, 1, 1)),
            Err(PixelBufferTooShort {
                required: 4,
                actual: 0
            })
        );
        assert!(extract_input_regions(&[], 0, 1, Region::default())
            .unwrap()
            .is_empty());
        let model = [Region::new(1, 2, 3, 4)];
        assert_eq!(frame_input_regions(false, 8, 9, &model), model);
        assert_eq!(
            frame_input_regions(true, 8, 9, &[]),
            [Region::new(0, 0, 8, 9)]
        );
    }
}
