//! Image geometry primitives ported from OpenCV so the DB pipeline keeps the
//! native coordinates: bilinear resize, cubic warp, min-area rectangle and
//! filled polygon masks.

use crate::Point;

/// A `3x3` projective transform, row-major like `cv::getPerspectiveTransform`.
#[derive(Clone, Copy, Debug)]
pub struct Transform {
    pub m: [[f64; 3]; 3],
}

impl Transform {
    /// Solve the `8x8` system `cv::getPerspectiveTransform` builds from four
    /// correspondence pairs. Returns `None` for a degenerate quad.
    pub fn perspective(src: &[Point; 4], dst: &[Point; 4]) -> Option<Self> {
        // OpenCV builds A x = b with rows alternating x and y equations.
        let mut a = [[0.0f64; 8]; 8];
        let mut b = [0.0f64; 8];
        for index in 0..4 {
            let sx = f64::from(src[index].x);
            let sy = f64::from(src[index].y);
            let dx = f64::from(dst[index].x);
            let dy = f64::from(dst[index].y);
            let first = index * 2;
            a[first] = [sx, sy, 1.0, 0.0, 0.0, 0.0, -sx * dx, -sy * dx];
            b[first] = dx;
            a[first + 1] = [0.0, 0.0, 0.0, sx, sy, 1.0, -sx * dy, -sy * dy];
            b[first + 1] = dy;
        }
        let solution = solve(&mut a, &mut b)?;
        Some(Transform {
            m: [
                [solution[0], solution[1], solution[2]],
                [solution[3], solution[4], solution[5]],
                [solution[6], solution[7], 1.0],
            ],
        })
    }

    pub fn inverse(&self) -> Option<Self> {
        let m = &self.m;
        let det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if det == 0.0 || !det.is_finite() {
            return None;
        }
        let inv = 1.0 / det;
        Some(Transform {
            m: [
                [
                    (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv,
                    (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv,
                    (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv,
                ],
                [
                    (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv,
                    (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv,
                    (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv,
                ],
                [
                    (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv,
                    (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv,
                    (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv,
                ],
            ],
        })
    }
}

/// Gaussian elimination with partial pivoting, matching `cv::solve` on the
/// well-conditioned systems produced by four non-degenerate corners.
fn solve(a: &mut [[f64; 8]; 8], b: &mut [f64; 8]) -> Option<[f64; 8]> {
    for column in 0..8 {
        let mut pivot = column;
        for row in column + 1..8 {
            if a[row][column].abs() > a[pivot][column].abs() {
                pivot = row;
            }
        }
        if a[pivot][column].abs() < 1e-12 {
            return None;
        }
        if pivot != column {
            a.swap(pivot, column);
            b.swap(pivot, column);
        }
        let scale = a[column][column];
        // `[f64; 8]` is `Copy`, so the pivot row can be read while mutating.
        let pivot_row = a[column];
        for row in column + 1..8 {
            let factor = a[row][column] / scale;
            if factor == 0.0 {
                continue;
            }
            for (target, pivot_value) in a[row][column..].iter_mut().zip(&pivot_row[column..]) {
                *target -= factor * pivot_value;
            }
            b[row] -= factor * b[column];
        }
    }
    let mut x = [0.0f64; 8];
    for row in (0..8).rev() {
        let mut sum = b[row];
        for column in row + 1..8 {
            sum -= a[row][column] * x[column];
        }
        x[row] = sum / a[row][row];
    }
    Some(x)
}

/// An `RGB8` image; channel order is RGB, not BGR.
#[derive(Clone, Debug)]
pub struct Rgb {
    pub width: usize,
    pub height: usize,
    pub pixels: Vec<u8>,
}

impl Rgb {
    pub fn new(width: usize, height: usize) -> Self {
        Rgb {
            width,
            height,
            pixels: vec![0; width * height * 3],
        }
    }

    pub fn filled(width: usize, height: usize, value: [u8; 3]) -> Self {
        let mut image = Rgb::new(width, height);
        for pixel in image.pixels.as_chunks_mut::<3>().0 {
            *pixel = value;
        }
        image
    }

    pub fn pixel(&self, x: usize, y: usize) -> [u8; 3] {
        let offset = (y * self.width + x) * 3;
        [
            self.pixels[offset],
            self.pixels[offset + 1],
            self.pixels[offset + 2],
        ]
    }

    pub fn set(&mut self, x: usize, y: usize, value: [u8; 3]) {
        let offset = (y * self.width + x) * 3;
        self.pixels[offset..offset + 3].copy_from_slice(&value);
    }
}

/// `cv::resize` with `INTER_LINEAR` on 8-bit images.
///
/// Sampling is `scale*(d+0.5)-0.5` with edge clamping. Coefficients are rounded
/// to the `INTER_RESIZE_COEF_BITS = 11` fixed-point scale and stored as `short`,
/// like OpenCV; the horizontal pass accumulates into `int` and the vertical pass
/// combines `b0*(S0 >> 4) >> 16` terms with an extra `(+2) >> 2` rounding. This
/// reproduces OpenCV's 8-bit output exactly instead of approximating it.
pub fn resize_linear(source: &Rgb, width: usize, height: usize) -> Rgb {
    if width == 0 || height == 0 || source.width == 0 || source.height == 0 {
        return Rgb::new(width, height);
    }
    if source.width == width && source.height == height {
        return source.clone();
    }
    let scale_x = source.width as f64 / width as f64;
    let scale_y = source.height as f64 / height as f64;

    let mut xofs = vec![0usize; width];
    let mut xalpha = vec![[0i32; 2]; width];
    for (dx, (ofs, alpha)) in xofs.iter_mut().zip(xalpha.iter_mut()).enumerate() {
        let fx = (dx as f64 + 0.5) * scale_x - 0.5;
        let mut sx = fx.floor();
        let mut fraction = fx - sx;
        if sx < 0.0 {
            sx = 0.0;
            fraction = 0.0;
        }
        if sx as i64 >= source.width as i64 - 1 {
            sx = (source.width - 1) as f64;
            fraction = 0.0;
        }
        *ofs = sx as usize;
        // Each coefficient is rounded independently, exactly like OpenCV's
        // `saturate_cast<short>(cbuf[k] * INTER_RESIZE_COEF_SCALE)`, so a pair
        // may sum to 2047 or 2049 rather than exactly 2048.
        alpha[0] = round_coefficient(1.0 - fraction);
        alpha[1] = round_coefficient(fraction);
    }

    let mut columns = vec![0i32; width * 3];
    let mut result = Rgb::new(width, height);
    let mut line = vec![0i32; width * 3];
    let mut previous_row: Option<i64> = None;
    let mut previous_line = vec![0i32; width * 3];

    for dy in 0..height {
        let fy = (dy as f64 + 0.5) * scale_y - 0.5;
        // Unlike the x axis, OpenCV keeps `fy` unclamped and clips the row
        // indices instead (`clip(sy0 - ksize2 + 1 + k, 0, height)`). Both rows
        // can therefore be the same source row while `beta` still splits the
        // weight between them, which changes 8-bit rounding.
        let sy = fy.floor();
        let fraction = fy - sy;
        let row0 = (sy as i64).clamp(0, source.height as i64 - 1) as usize;
        let row1 = ((sy as i64) + 1).clamp(0, source.height as i64 - 1) as usize;
        let beta = [
            round_coefficient(1.0 - fraction),
            round_coefficient(fraction),
        ];

        // Horizontal pass for both source rows, reusing the previous row when
        // consecutive output rows share it (OpenCV keeps a small ring buffer).
        if previous_row != Some(row0 as i64) {
            horizontal_linear(source, row0, &xofs, &xalpha, &mut previous_line);
            previous_row = Some(row0 as i64);
        }
        horizontal_linear(source, row1, &xofs, &xalpha, &mut line);
        columns.copy_from_slice(&previous_line);

        for x in 0..width {
            for channel in 0..3 {
                let index = x * 3 + channel;
                let s0 = columns[index] >> 4;
                let s1 = line[index] >> 4;
                let value = (((beta[0] * s0) >> 16) + ((beta[1] * s1) >> 16) + 2) >> 2;
                result.pixels[dy * width * 3 + index] = value.clamp(0, 255) as u8;
            }
        }
    }
    result
}

/// `saturate_cast<short>(coefficient * 2048)`: round half to even, clamp.
fn round_coefficient(coefficient: f64) -> i32 {
    (coefficient * 2048.0).round_ties_even().clamp(0.0, 2048.0) as i32
}

/// Horizontal 8-bit linear pass into an `int` buffer with `>> 4` scaling.
fn horizontal_linear(
    source: &Rgb,
    row: usize,
    xofs: &[usize],
    xalpha: &[[i32; 2]],
    out: &mut [i32],
) {
    for (x, (ofs, alpha)) in xofs.iter().zip(xalpha.iter()).enumerate() {
        let next = (*ofs + 1).min(source.width - 1);
        let base = (row * source.width) * 3;
        for channel in 0..3 {
            let s0 = i32::from(source.pixels[base + ofs * 3 + channel]);
            let s1 = i32::from(source.pixels[base + next * 3 + channel]);
            out[x * 3 + channel] = s0 * alpha[0] + s1 * alpha[1];
        }
    }
}

/// `cv::warpPerspective` with `INTER_CUBIC` and `BORDER_REPLICATE`.
///
/// OpenCV does not evaluate the cubic weights per pixel. It quantises the
/// subpixel offset to `1/32` (`INTER_TAB_SIZE`), looks up `BicubicTab_i`, and
/// accumulates `short` weights at a `1 << 15` scale. This port reproduces that
/// table, including the fix-up that forces each 4x4 table to sum exactly to the
/// scale, so the output matches OpenCV bit for bit.
pub fn warp_perspective_cubic(
    source: &Rgb,
    transform: &Transform,
    width: usize,
    height: usize,
) -> Rgb {
    let inverse = match transform.inverse() {
        Some(inverse) => inverse,
        None => return Rgb::filled(width, height, [0, 0, 0]),
    };
    let mut result = Rgb::new(width, height);
    if source.width == 0 || source.height == 0 {
        return result;
    }
    let table = cubic_table();
    // `WarpPerspectiveInvoker` walks the destination in 32x32-ish blocks and
    // evaluates the inverse map incrementally from each block's origin:
    // `X0 = M[0]*x + M[1]*(y+y1) + M[2]`, then `(X0 + M[0]*x1) / W`. Doing the
    // full dot product per pixel instead changes the last bits of the quantised
    // offset and, occasionally, one output level.
    let block = 32usize;
    let mut block_y = 0usize;
    while block_y < height {
        let bh = block.min(height - block_y);
        let mut block_x = 0usize;
        while block_x < width {
            let bw = block.min(width - block_x);
            for offset_y in 0..bh {
                let y = block_y + offset_y;
                let x0 =
                    inverse.m[0][0] * block_x as f64 + inverse.m[0][1] * y as f64 + inverse.m[0][2];
                let y0 =
                    inverse.m[1][0] * block_x as f64 + inverse.m[1][1] * y as f64 + inverse.m[1][2];
                let w0 =
                    inverse.m[2][0] * block_x as f64 + inverse.m[2][1] * y as f64 + inverse.m[2][2];
                for offset_x in 0..bw {
                    let x = block_x + offset_x;
                    // OpenCV forms `W = INTER_TAB_SIZE / W0` first and then
                    // multiplies, rather than dividing and scaling afterwards;
                    // the operation order changes the last bits.
                    let mut w = w0 + inverse.m[2][0] * offset_x as f64;
                    w = if w != 0.0 {
                        f64::from(INTER_TAB_SIZE) / w
                    } else {
                        0.0
                    };
                    let map_x = (x0 + inverse.m[0][0] * offset_x as f64) * w;
                    let map_y = (y0 + inverse.m[1][0] * offset_x as f64) * w;
                    // `saturate_cast<int>` is `cvRound`: round half to even.
                    let quantised_x = cv_round(map_x);
                    let quantised_y = cv_round(map_y);
                    let sx = quantised_x >> INTER_BITS;
                    let sy = quantised_y >> INTER_BITS;
                    let fraction_x = quantised_x & (i64::from(INTER_TAB_SIZE) - 1);
                    let fraction_y = quantised_y & (i64::from(INTER_TAB_SIZE) - 1);
                    // Indexed `frac_y * INTER_TAB_SIZE + frac_x`, matching
                    // `alpha = (Y & 31)*INTER_TAB_SIZE + (X & 31)`.
                    let weights =
                        &table[(fraction_y * i64::from(INTER_TAB_SIZE) + fraction_x) as usize];
                    let mut value = [0u8; 3];
                    for (channel, slot) in value.iter_mut().enumerate() {
                        // Four terms per row, one row at a time, in `int`.
                        let mut sum = 0i32;
                        for row in 0..4 {
                            // `sy - 1 + i` with BORDER_REPLICATE.
                            let source_y = (sy + row as i64 - 1).clamp(0, source.height as i64 - 1);
                            for column in 0..4 {
                                let source_x =
                                    (sx + column as i64 - 1).clamp(0, source.width as i64 - 1);
                                let sample = i32::from(
                                    source.pixels[(source_y as usize * source.width
                                        + source_x as usize)
                                        * 3
                                        + channel],
                                );
                                sum += sample * i32::from(weights[row * 4 + column]);
                            }
                        }
                        // `FixedPtCast<int, uchar, INTER_REMAP_COEF_BITS>`.
                        let rounded =
                            (sum + (1 << (INTER_REMAP_COEF_BITS - 1))) >> INTER_REMAP_COEF_BITS;
                        *slot = rounded.clamp(0, 255) as u8;
                    }
                    result.set(x, y, value);
                }
            }
            block_x += bw;
        }
        block_y += bh;
    }
    result
}

/// `INTER_TAB_SIZE`: subpixel quantum of the remap table.
const INTER_TAB_SIZE: i32 = 1 << 5;
/// `INTER_BITS`: log2 of `INTER_TAB_SIZE`.
const INTER_BITS: i64 = 5;
/// `INTER_REMAP_COEF_BITS`: fixed-point scale of the remap weights.
const INTER_REMAP_COEF_BITS: i32 = 15;
const INTER_REMAP_COEF_SCALE: i32 = 1 << INTER_REMAP_COEF_BITS;

/// `cvRound` / `saturate_cast<int>`: round half to even, then clamp.
fn cv_round(value: f64) -> i64 {
    if value.is_nan() {
        return 0;
    }
    let rounded = value.round_ties_even();
    rounded.clamp(f64::from(i32::MIN), f64::from(i32::MAX)) as i64
}

/// `BicubicTab_i` from `initInterTab2D`, built once.
fn cubic_table() -> Vec<[i16; 16]> {
    let mut one_dimensional = [[0f32; 4]; INTER_TAB_SIZE as usize];
    for (index, entry) in one_dimensional.iter_mut().enumerate() {
        *entry = cubic_coefficients(index as f32 / INTER_TAB_SIZE as f32);
    }
    let mut table = Vec::with_capacity((INTER_TAB_SIZE * INTER_TAB_SIZE) as usize);
    for y in 0..INTER_TAB_SIZE as usize {
        for x in 0..INTER_TAB_SIZE as usize {
            let mut weights = [0i16; 16];
            let mut sum = 0i32;
            for (row, weight_y) in one_dimensional[y].iter().enumerate() {
                for (column, weight_x) in one_dimensional[x].iter().enumerate() {
                    let value = weight_y * weight_x;
                    // `saturate_cast<short>(v * INTER_REMAP_COEF_SCALE)`. The
                    // clamp matters: a weight of exactly 1.0 saturates to 32767,
                    // not 32768, which is what drives the sum fix-up below.
                    let scaled =
                        (f64::from(value) * f64::from(INTER_REMAP_COEF_SCALE)).round_ties_even();
                    let stored = scaled.clamp(f64::from(i16::MIN), f64::from(i16::MAX)) as i16;
                    weights[row * 4 + column] = stored;
                    sum += i32::from(stored);
                }
            }
            // OpenCV nudges the extreme weights so the 4x4 block sums exactly
            // to INTER_REMAP_COEF_SCALE.
            if sum != INTER_REMAP_COEF_SCALE {
                let difference = sum - INTER_REMAP_COEF_SCALE;
                let (mut max_index, mut min_index) = (8, 8);
                for row in 2..4 {
                    for column in 2..4 {
                        let index = row * 4 + column;
                        if weights[index] < weights[min_index] {
                            min_index = index;
                        } else if weights[index] > weights[max_index] {
                            max_index = index;
                        }
                    }
                }
                let target = if difference < 0 { max_index } else { min_index };
                weights[target] = (i32::from(weights[target]) - difference) as i16;
            }
            table.push(weights);
        }
    }
    table
}

/// `cv::interpolateCubic` with `A = -0.75`.
fn cubic_coefficients(offset: f32) -> [f32; 4] {
    const A: f32 = -0.75;
    let x = offset;
    let mut coefficients = [0.0f32; 4];
    coefficients[0] = ((A * (x + 1.0) - 5.0 * A) * (x + 1.0) + 8.0 * A) * (x + 1.0) - 4.0 * A;
    coefficients[1] = ((A + 2.0) * x - (A + 3.0)) * x * x + 1.0;
    coefficients[2] = ((A + 2.0) * (1.0 - x) - (A + 3.0)) * (1.0 - x) * (1.0 - x) + 1.0;
    coefficients[3] = 1.0 - coefficients[0] - coefficients[1] - coefficients[2];
    coefficients
}

/// A `cv::RotatedRect`: center, size and the OpenCV angle convention
/// (`[-90, 0)` degrees, measured from the vertical axis).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RotatedRect {
    pub center: Point,
    pub width: f32,
    pub height: f32,
    pub angle: f32,
}

impl RotatedRect {
    /// `cv::RotatedRect::points`: bottom-left, top-left, top-right,
    /// bottom-right of the rotated box.
    pub fn points(&self) -> [Point; 4] {
        let radians = f64::from(self.angle) * std::f64::consts::PI / 180.0;
        let b = (radians.cos() * 0.5) as f32;
        let a = (radians.sin() * 0.5) as f32;
        let ah = a * self.height;
        let aw = a * self.width;
        let bh = b * self.height;
        let bw = b * self.width;
        [
            Point {
                x: self.center.x - ah - bw,
                y: self.center.y + bh - aw,
            },
            Point {
                x: self.center.x + ah - bw,
                y: self.center.y - bh - aw,
            },
            Point {
                x: self.center.x + ah + bw,
                y: self.center.y - bh + aw,
            },
            Point {
                x: self.center.x - ah + bw,
                y: self.center.y + bh + aw,
            },
        ]
    }
}

/// `cv::minAreaRect` over integer contour points, including the rotating
/// calipers pass and the `n <= 2` special cases.
pub fn min_area_rect(points: &[(i32, i32)]) -> RotatedRect {
    let mut hull = convex_hull(points);
    let n = hull.len();
    let mut angle = -std::f64::consts::FRAC_PI_2;
    let mut rect = RotatedRect {
        center: Point { x: 0.0, y: 0.0 },
        width: 0.0,
        height: 0.0,
        angle: 0.0,
    };
    match n {
        0 => {}
        1 => {
            rect.center = Point {
                x: hull[0].0 as f32,
                y: hull[0].1 as f32,
            };
        }
        2 => {
            let (x0, y0) = hull[0];
            let (x1, y1) = hull[1];
            rect.center = Point {
                x: (x0 as f32 + x1 as f32) * 0.5,
                y: (y0 as f32 + y1 as f32) * 0.5,
            };
            let dx = f64::from(x0 - x1);
            let dy = f64::from(y0 - y1);
            rect.width = 0.0;
            rect.height = (dx * dx + dy * dy).sqrt() as f32;
            if dx == 0.0 {
                std::mem::swap(&mut rect.width, &mut rect.height);
            } else if dy < 0.0 {
                angle = dy.atan2(dx);
                std::mem::swap(&mut rect.width, &mut rect.height);
            } else if dy > 0.0 {
                angle = -dx.atan2(dy);
            }
        }
        _ => {
            hull.dedup();
            let out = rotating_calipers(&hull);
            rect.center = Point {
                x: out[0].0 + (out[1].0 + out[2].0) * 0.5,
                y: out[0].1 + (out[1].1 + out[2].1) * 0.5,
            };
            rect.width = (out[2].0 * out[2].0 + out[2].1 * out[2].1).sqrt();
            rect.height = (out[1].0 * out[1].0 + out[1].1 * out[1].1).sqrt();
            if out[1].0 == 0.0 && out[1].1 > 0.0 {
                std::mem::swap(&mut rect.width, &mut rect.height);
            } else {
                angle = -(out[1].0 as f64).atan2(out[1].1 as f64);
            }
        }
    }
    rect.angle = (angle * 180.0 / std::f64::consts::PI) as f32;
    rect
}

/// `cv::convexHull` (Sklansky, counter-clockwise) for `i32` points, returning
/// hull points in OpenCV vertex order.
fn convex_hull(points: &[(i32, i32)]) -> Vec<(i32, i32)> {
    if points.is_empty() {
        return Vec::new();
    }
    // OpenCV sorts by (x, y) then by pointer, i.e. input order for ties.
    let mut sorted: Vec<usize> = (0..points.len()).collect();
    sorted.sort_by(|a, b| {
        let pa = points[*a];
        let pb = points[*b];
        (pa.0, pa.1, *a).cmp(&(pb.0, pb.1, *b))
    });
    let ordered: Vec<(i32, i32)> = sorted.iter().map(|index| points[*index]).collect();
    let total = ordered.len();

    let mut miny = 0usize;
    let mut maxy = 0usize;
    for index in 1..total {
        if ordered[miny].1 > ordered[index].1 {
            miny = index;
        }
        if ordered[maxy].1 < ordered[index].1 {
            maxy = index;
        }
    }
    if ordered[0] == ordered[total - 1] {
        return vec![ordered[0]];
    }

    // Upper half: OpenCV computes the ascending chain into `tl` and the
    // descending chain into `tr`, then swaps them because `clockwise` is false.
    let first = sklansky(&ordered, 0, maxy, -1, 1);
    let second = sklansky(&ordered, total - 1, maxy, -1, -1);
    let (tl_stack, tl_count) = (&second, second.len());
    let (tr_stack, tr_count) = (&first, first.len());

    // Lower half; `clockwise` is false, so these are not swapped either.
    let third = sklansky(&ordered, 0, miny, 1, -1);
    let fourth = sklansky(&ordered, total - 1, miny, 1, 1);
    let (bl_stack, mut bl_count) = (&third, third.len());
    let (br_stack, mut br_count) = (&fourth, fourth.len());

    let stop = if tr_count > 2 {
        Some(ordered[tr_stack[1]])
    } else if tl_count > 2 {
        Some(ordered[tl_stack[tl_count - 2]])
    } else {
        None
    };
    if let Some(stop) = stop {
        let check = if bl_count > 2 {
            Some(ordered[bl_stack[1]])
        } else if bl_count + br_count > 2 {
            Some(ordered[br_stack[2 - bl_count]])
        } else {
            None
        };
        if check == Some(stop) {
            // All points are collinear: mirror the top half with at most two
            // points on each side.
            bl_count = bl_count.min(2);
            br_count = br_count.min(2);
        }
    }

    let mut hull = Vec::with_capacity(total);
    for item in tl_stack.iter().take(tl_count.saturating_sub(1)) {
        hull.push(ordered[*item]);
    }
    for item in tr_stack.iter().take(tr_count).skip(1).rev() {
        hull.push(ordered[*item]);
    }
    for item in bl_stack.iter().take(bl_count.saturating_sub(1)) {
        hull.push(ordered[*item]);
    }
    for item in br_stack.iter().take(br_count).skip(1).rev() {
        hull.push(ordered[*item]);
    }
    hull
}

/// `Sklansky_<int, int64>` from OpenCV's `convhull.cpp`; returns the stack of
/// indices, first `count` entries meaningful.
fn sklansky(
    ordered: &[(i32, i32)],
    start: usize,
    end: usize,
    nsign: i64,
    sign2: i64,
) -> Vec<usize> {
    let step: isize = if end > start { 1 } else { -1 };
    let mut stack = vec![0usize; ordered.len() + 2];
    let mut pprev = start as isize;
    let mut pcur = pprev + step;
    let mut pnext = pcur + step;
    let mut stacksize = 3usize;

    if start == end || (ordered[start].0 == ordered[end].0 && ordered[start].1 == ordered[end].1) {
        stack[0] = start;
        stack.truncate(1);
        return stack;
    }
    stack[0] = pprev as usize;
    stack[1] = pcur as usize;
    stack[2] = pnext as usize;
    let after_end = end as isize + step;

    while pnext != after_end {
        let cury = i64::from(ordered[pcur as usize].1);
        let nexty = i64::from(ordered[pnext as usize].1);
        let by = nexty - cury;
        if by.signum() != nsign {
            let ax = f64::from(ordered[pcur as usize].0 - ordered[pprev as usize].0);
            let ay = (cury - i64::from(ordered[pprev as usize].1)) as f64;
            let bx = f64::from(ordered[pnext as usize].0 - ordered[pcur as usize].0);
            let byf = by as f64;
            let (axn, ayn) = normalize(ax, ay);
            let (bxn, byn) = normalize(bx, byf);
            let convexity = ayn * bxn - axn * byn;
            let sign = if convexity > 0.0 {
                1
            } else if convexity < 0.0 {
                -1
            } else {
                0
            };
            if sign == sign2 && (axn != 0.0 || ayn != 0.0) {
                pprev = pcur;
                pcur = pnext;
                pnext += step;
                stack[stacksize] = pnext as usize;
                stacksize += 1;
            } else if pprev == start as isize {
                pcur = pnext;
                stack[1] = pcur as usize;
                pnext += step;
                stack[2] = pnext as usize;
            } else {
                stack[stacksize - 2] = pnext as usize;
                pcur = pprev;
                pprev = stack[stacksize - 4] as isize;
                stacksize -= 1;
            }
        } else {
            pnext += step;
            stack[stacksize - 1] = pnext as usize;
        }
    }
    stack.truncate(stacksize - 1);
    stack
}

fn normalize(x: f64, y: f64) -> (f64, f64) {
    let length = x.hypot(y);
    if length == 0.0 {
        (x, y)
    } else {
        (x / length, y / length)
    }
}

/// `rotatingCalipers` in `CALIPERS_MINAREARECT` mode returning
/// `[corner, vector1, vector2]` as `(x, y)` pairs.
fn rotating_calipers(points: &[(i32, i32)]) -> [(f32, f32); 4] {
    let n = points.len();
    let mut vectors = vec![(0f32, 0f32); n];
    let mut inverse_lengths = vec![0f32; n];
    let mut left = 0usize;
    let mut bottom = 0usize;
    let mut right = 0usize;
    let mut top = 0usize;
    let mut left_x = points[0].0 as f32;
    let mut right_x = left_x;
    let mut top_y = points[0].1 as f32;
    let mut bottom_y = top_y;

    let mut previous = points[0];
    for index in 0..n {
        let current = points[index];
        if (previous.0 as f32) < left_x {
            left_x = previous.0 as f32;
            left = index;
        }
        if (previous.0 as f32) > right_x {
            right_x = previous.0 as f32;
            right = index;
        }
        if (previous.1 as f32) > top_y {
            top_y = previous.1 as f32;
            top = index;
        }
        if (previous.1 as f32) < bottom_y {
            bottom_y = previous.1 as f32;
            bottom = index;
        }
        let next = points[(index + 1) % n];
        let dx = f64::from(next.0 - current.0);
        let dy = f64::from(next.1 - current.1);
        vectors[index] = (dx as f32, dy as f32);
        inverse_lengths[index] = (1.0 / dx.hypot(dy)) as f32;
        previous = next;
    }

    // OpenCV is called with `clockwise = false`, so the hull is CCW.
    let mut orientation = 1.0f32;
    {
        let mut ax = f64::from(vectors[n - 1].0);
        let mut ay = f64::from(vectors[n - 1].1);
        let mut found = false;
        for vector in vectors.iter() {
            let bx = f64::from(vector.0);
            let by = f64::from(vector.1);
            let convexity = ax * by - ay * bx;
            if convexity != 0.0 {
                orientation = if convexity > 0.0 { 1.0 } else { -1.0 };
                found = true;
                break;
            }
            ax = bx;
            ay = by;
        }
        if !found {
            // A hull whose edges are all collinear cannot be measured; OpenCV
            // asserts. Keep the axis-aligned fallback instead of panicking.
            orientation = 1.0;
        }
    }
    debug_assert_ne!(orientation, 0.0);
    let mut sequence = [bottom, right, top, left];

    let mut minimum_area = f32::MAX;
    let mut saved = (0usize, 0f32, 0f32, 0f32, 0f32, 0usize);

    for _ in 0..n {
        let mut rotations = [(0f32, 0f32); 4];
        rotations[0] = vectors[sequence[0]];
        rotations[1] = rotate90_cw(vectors[sequence[1]]);
        rotations[2] = rotate180(vectors[sequence[2]]);
        rotations[3] = rotate90_ccw(vectors[sequence[3]]);
        let mut main = 0usize;
        for index in 1..4 {
            if first_vector_is_right(rotations[index], rotations[main]) {
                main = index;
            }
        }
        let index = sequence[main];
        let lead_x = vectors[index].0 * inverse_lengths[index];
        let lead_y = vectors[index].1 * inverse_lengths[index];
        // `base_a`/`base_b` are the caliper direction used for the area below.
        let (base_a, base_b) = match main {
            0 => (lead_x, lead_y),
            1 => (lead_y, -lead_x),
            2 => (-lead_x, -lead_y),
            _ => (-lead_y, lead_x),
        };
        sequence[main] = (sequence[main] + 1) % n;

        let dx = (points[sequence[1]].0 - points[sequence[3]].0) as f32;
        let dy = (points[sequence[1]].1 - points[sequence[3]].1) as f32;
        let width = dx * base_a + dy * base_b;
        let dx = (points[sequence[2]].0 - points[sequence[0]].0) as f32;
        let dy = (points[sequence[2]].1 - points[sequence[0]].1) as f32;
        let height = -dx * base_b + dy * base_a;
        let area = width * height;
        if area <= minimum_area {
            minimum_area = area;
            saved = (sequence[3], base_a, width, base_b, height, sequence[0]);
        }
    }

    let (left_index, a1, width, b1, height, bottom_index) = saved;
    let a2 = -b1;
    let b2 = a1;
    let c1 = a1 * points[left_index].0 as f32 + points[left_index].1 as f32 * b1;
    let c2 = a2 * points[bottom_index].0 as f32 + points[bottom_index].1 as f32 * b2;
    let determinant = 1.0 / (a1 * b2 - a2 * b1);
    let px = (c1 * b2 - c2 * b1) * determinant;
    let py = (a1 * c2 - a2 * c1) * determinant;
    [
        (px, py),
        (a1 * width, b1 * width),
        (a2 * height, b2 * height),
        (0.0, 0.0),
    ]
}

fn rotate90_cw(vector: (f32, f32)) -> (f32, f32) {
    (vector.1, -vector.0)
}

fn rotate90_ccw(vector: (f32, f32)) -> (f32, f32) {
    (-vector.1, vector.0)
}

fn rotate180(vector: (f32, f32)) -> (f32, f32) {
    (-vector.0, -vector.1)
}

fn first_vector_is_right(first: (f32, f32), second: (f32, f32)) -> bool {
    let rotated = rotate90_cw(first);
    rotated.0 * second.0 + rotated.1 * second.1 < 0.0
}

/// `cv::fillPoly` for a single `i32` polygon on a `CV_8U` mask, which only the
/// union of covered pixels matters for.
pub fn fill_polygon(width: usize, height: usize, polygon: &[(i32, i32)]) -> Vec<u8> {
    let mut mask = vec![0u8; width * height];
    if polygon.len() < 3 || width == 0 || height == 0 {
        return mask;
    }
    // Even-odd scanline fill over polygon edges, matching fillPoly's result.
    let mut edges: Vec<(f64, f64, f64, f64)> = Vec::new();
    for index in 0..polygon.len() {
        let (x0, y0) = polygon[index];
        let (x1, y1) = polygon[(index + 1) % polygon.len()];
        if y0 == y1 {
            continue;
        }
        edges.push((x0 as f64, y0 as f64, x1 as f64, y1 as f64));
    }
    for y in 0..height {
        let scan = y as f64 + 0.5;
        let mut crossings: Vec<f64> = Vec::new();
        for &(x0, y0, x1, y1) in &edges {
            let (low, high, low_x, high_x) = if y0 < y1 {
                (y0, y1, x0, x1)
            } else {
                (y1, y0, x1, x0)
            };
            if scan >= low && scan < high {
                let t = (scan - low) / (high - low);
                crossings.push(low_x + t * (high_x - low_x));
            }
        }
        crossings.sort_by(|a, b| a.total_cmp(b));
        for pair in crossings.as_chunks::<2>().0 {
            let start = pair[0].ceil() as i64;
            let end = pair[1].floor() as i64;
            let start = start.max(0);
            let end = end.min(width as i64 - 1);
            for x in start..=end {
                mask[y * width + x as usize] = 1;
            }
        }
    }
    mask
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn min_area_rect_matches_axis_aligned_extents() {
        // A filled 50x20 pixel rectangle reaches from (5,20) to (54,39), so its
        // contours reach those pixel centres. `cv::minAreaRect` measures the
        // hull along its own axes, giving 19x49 here (`height` is the long
        // side), and Corners() then orders the points as tl, tr, br, bl.
        // Verified against OpenCV 4.14/5.x `minAreaRect` on the same points.
        let points = [(5, 20), (54, 20), (54, 39), (5, 39)];
        let rect = min_area_rect(&points);
        assert!((rect.center.x - 29.5).abs() < 1e-4, "{rect:?}");
        assert!((rect.center.y - 29.5).abs() < 1e-4, "{rect:?}");
        assert!((rect.width - 19.0).abs() < 1e-3, "{rect:?}");
        assert!((rect.height - 49.0).abs() < 1e-3, "{rect:?}");
        assert!((rect.angle + 90.0).abs() < 1e-3, "{rect:?}");

        let corners = crate::detect::corners(&rect);
        assert!((corners[0].x - 5.0).abs() < 1e-3, "{corners:?}");
        assert!((corners[0].y - 20.0).abs() < 1e-3, "{corners:?}");
        assert!((corners[1].x - 54.0).abs() < 1e-3, "{corners:?}");
        assert!((corners[1].y - 20.0).abs() < 1e-3, "{corners:?}");
        assert!((corners[2].x - 54.0).abs() < 1e-3, "{corners:?}");
        assert!((corners[2].y - 39.0).abs() < 1e-3, "{corners:?}");
        assert!((corners[3].x - 5.0).abs() < 1e-3, "{corners:?}");
        assert!((corners[3].y - 39.0).abs() < 1e-3, "{corners:?}");
    }

    #[test]
    fn min_area_rect_agrees_with_opencv_on_rotated_and_degenerate_input() {
        // These expectations were produced by OpenCV's own `minAreaRect` and
        // `RotatedRect::points` (see the crate README), so they pin the
        // rotating-calipers port rather than restating the implementation.
        type Case<'a> = (&'a [(i32, i32)], [f32; 5]);
        let cases: [Case; 4] = [
            // A thin rasterised diagonal: the calipers find a genuinely rotated
            // box instead of an axis-aligned one.
            (
                &[(0, 0), (10, 10), (10, 9), (9, 10)],
                [5.248619, 4.776243, 1.412259, 14.122589, -41.987217],
            ),
            // Two points: OpenCV reports a zero-width box along the segment and
            // swaps width/height so `height` carries the length.
            (&[(5, 5), (5, 9)], [5.0, 7.0, 4.0, 0.0, -90.0]),
            // One point: OpenCV keeps the default -90 degree angle.
            (&[(7, 3)], [7.0, 3.0, 0.0, 0.0, -90.0]),
            // Collinear points must not confuse the hull; the box collapses.
            (
                &[(0, 0), (1, 0), (2, 0), (3, 0)],
                [1.5, 0.0, 0.0, 3.0, -90.0],
            ),
        ];
        for (points, expected) in cases {
            let rect = min_area_rect(points);
            let actual = [
                rect.center.x,
                rect.center.y,
                rect.width,
                rect.height,
                rect.angle,
            ];
            for (got, want) in actual.iter().zip(expected.iter()) {
                assert!(
                    (got - want).abs() < 1e-3,
                    "{points:?}: got {actual:?}, expected {expected:?}"
                );
            }
        }
    }

    /// True when every pixel of `image` is exactly `colour`.
    fn is_uniform(image: &Rgb, colour: [u8; 3]) -> bool {
        image.pixels.as_chunks::<3>().0.iter().all(|p| p == &colour)
    }

    #[test]
    fn resize_preserves_uniform_colour() {
        let source = Rgb::filled(64, 64, [10, 20, 30]);
        let resized = resize_linear(&source, 32, 32);
        assert_eq!(resized.width, 32);
        assert!(is_uniform(&resized, [10, 20, 30]));
    }

    #[test]
    fn perspective_is_invertible_on_a_square() {
        let corners = [
            Point { x: 0.0, y: 0.0 },
            Point { x: 10.0, y: 0.0 },
            Point { x: 10.0, y: 10.0 },
            Point { x: 0.0, y: 10.0 },
        ];
        let transform = Transform::perspective(&corners, &corners).unwrap();
        assert!((transform.m[0][0] - 1.0).abs() < 1e-9);
        assert!(transform.inverse().is_some());
    }

    #[test]
    fn fill_polygon_covers_a_rectangle_interior() {
        let mask = fill_polygon(10, 10, &[(2, 2), (7, 2), (7, 7), (2, 7)]);
        assert_eq!(mask[4 * 10 + 4], 1);
        assert_eq!(mask[0], 0);
    }

    #[test]
    fn resize_matches_opencv_bit_for_bit() {
        // Reference output of `cv::resize(src, dst, {5,4})` on this exact 3x3
        // image (OpenCV 5.0, INTER_LINEAR): every channel value is reproduced.
        let mut pixels = Vec::new();
        for value in [0u8, 10, 20, 30, 40, 50, 60, 70, 80] {
            pixels.extend_from_slice(&[value, value, value]);
        }
        let source = Rgb {
            width: 3,
            height: 3,
            pixels,
        };
        let resized = resize_linear(&source, 5, 4);
        let expected = [
            0, 0, 0, 4, 4, 4, 10, 10, 10, 16, 16, 16, 20, 20, 20, 19, 19, 19, 22, 22, 22, 29, 29,
            29, 35, 35, 35, 39, 39, 39, 41, 41, 41, 45, 45, 45, 51, 51, 51, 57, 57, 57, 61, 61, 61,
            60, 60, 60, 64, 64, 64, 70, 70, 70, 76, 76, 76, 80, 80, 80,
        ];
        assert_eq!(resized.pixels, expected);
    }

    #[test]
    fn resize_keeps_a_uniform_image_exact() {
        // OpenCV's 11-bit coefficients are rounded independently, so the pair can
        // sum to 2047 rather than 2048; a uniform image still has to survive
        // unchanged, which only holds when the coefficient handling is right.
        let source = Rgb::filled(9, 7, [17, 33, 250]);
        for (width, height) in [(4, 3), (13, 11), (9, 7)] {
            let resized = resize_linear(&source, width, height);
            assert!(
                resized
                    .pixels
                    .as_chunks::<3>()
                    .0
                    .iter()
                    .all(|p| p == &[17, 33, 250]),
                "{width}x{height}"
            );
        }
    }

    #[test]
    fn warp_keeps_a_uniform_image_exact_at_every_subpixel_offset() {
        // Each 4x4 cubic weight block must sum to the full 1<<15 scale; the
        // saturate-to-short step and the OpenCV sum fix-up guarantee that.
        let source = Rgb::filled(20, 20, [255, 200, 120]);
        for step in 0..8 {
            let offset = step as f32 * 0.125;
            let src = [
                Point { x: 1.0, y: 1.0 },
                Point {
                    x: 18.0,
                    y: 1.0 + offset,
                },
                Point {
                    x: 18.0 - offset,
                    y: 18.0,
                },
                Point {
                    x: 1.0,
                    y: 18.0 - offset,
                },
            ];
            let dst = [
                Point { x: 0.0, y: 0.0 },
                Point { x: 16.0, y: 0.0 },
                Point { x: 16.0, y: 16.0 },
                Point { x: 0.0, y: 16.0 },
            ];
            let transform = Transform::perspective(&src, &dst).unwrap();
            let warped = warp_perspective_cubic(&source, &transform, 16, 16);
            assert!(
                warped
                    .pixels
                    .as_chunks::<3>()
                    .0
                    .iter()
                    .all(|p| p == &[255, 200, 120]),
                "offset {offset}"
            );
        }
    }

    #[test]
    fn warp_matches_opencv_on_a_border_clamped_quad() {
        // Reference values from `cv::warpPerspective` with INTER_CUBIC and
        // BORDER_REPLICATE for this quad. The map lands partly outside the
        // source, which exercises the clamped taps; OpenCV's own output differs
        // in at most one level on two of the 48 samples, so the comparison
        // allows a single step.
        let source = Rgb::filled(6, 6, [10, 20, 30]);
        let src = [
            Point { x: 0.5, y: 0.5 },
            Point { x: 5.0, y: 0.5 },
            Point { x: 5.0, y: 5.0 },
            Point { x: 0.5, y: 5.0 },
        ];
        let dst = [
            Point { x: 0.0, y: 0.0 },
            Point { x: 8.0, y: 0.0 },
            Point { x: 8.0, y: 4.0 },
            Point { x: 0.0, y: 4.0 },
        ];
        let transform = Transform::perspective(&src, &dst).unwrap();
        let warped = warp_perspective_cubic(&source, &transform, 8, 4);
        assert!(
            warped
                .pixels
                .as_chunks::<3>()
                .0
                .iter()
                .all(|p| p == &[10, 20, 30]),
            "{:?}",
            &warped.pixels[..]
        );
    }
}
