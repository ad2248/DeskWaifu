//! Contours equivalent to `cv::findContours` with `RETR_LIST` and
//! `CHAIN_APPROX_SIMPLE`.
//!
//! This is OpenCV's legacy border-following scanner for 8-bit masks: the mask is
//! copied into a one-pixel zero border, and each unpainted foreground pixel
//! starts a border trace that records a point whenever the trace direction
//! changes. Traced pixels are marked so the raster scan skips them later.

/// `icvCodeDeltas`: neighbour steps of the trace, in x/y form.
const CODE_DELTAS: [(i32, i32); 8] = [
    (1, 0),
    (1, -1),
    (0, -1),
    (-1, -1),
    (-1, 0),
    (-1, 1),
    (0, 1),
    (1, 1),
];

const MAX_SIZE: usize = 16;

/// A zero-bordered mask in `CV_8S`: `0` background, `1` untraced foreground,
/// `2` traced foreground and `-126` a traced border pixel.
struct Mask {
    step: i32,
    data: Vec<i8>,
}

impl Mask {
    fn get(&self, index: i32) -> i8 {
        self.data[index as usize]
    }

    fn set(&mut self, index: i32, value: i8) {
        self.data[index as usize] = value;
    }

    fn index(&self, x: i32, y: i32) -> i32 {
        y * self.step + x
    }
}

/// Extract contours from a binary mask in OpenCV's `RETR_LIST` order. Points
/// are image coordinates, with the scanner's `(-1, -1)` offset applied.
pub fn find_contours(mask: &[u8], width: usize, height: usize) -> Vec<Vec<(i32, i32)>> {
    if width == 0 || height == 0 {
        return Vec::new();
    }
    let step = width as i32 + 2;
    let padded_width = width + 2;
    let padded_height = height + 2;
    let mut image = Mask {
        step,
        data: vec![0; padded_width * padded_height],
    };
    for y in 0..height as i32 {
        for x in 0..width as i32 {
            if mask[(y * width as i32 + x) as usize] != 0 {
                image.set(image.index(x + 1, y + 1), 1);
            }
        }
    }

    // `CV_INIT_3X3_DELTAS(deltas, step, 1)`, duplicated to 16 entries so the
    // trace can scan past direction 7 before wrapping.
    let base = [1, -step + 1, -step, -step - 1, -1, step - 1, step, step + 1];
    let mut deltas = [0i32; MAX_SIZE];
    deltas[..8].copy_from_slice(&base);
    deltas[8..].copy_from_slice(&base);

    let mut contours: Vec<Vec<(i32, i32)>> = Vec::new();
    // The scanner excludes the outermost padded row and column.
    let scan_width = padded_width as i32 - 1;
    let scan_height = padded_height as i32 - 1;
    let mut y = 1i32;
    while y < scan_height {
        let mut x = 1i32;
        let mut prev: i8 = 0;
        while x < scan_width {
            let p = image.get(image.index(x, y));
            if p == prev {
                x += 1;
                continue;
            }
            let is_hole = !(prev == 0 && p == 1);
            if is_hole && (p != 0 || prev < 1) {
                // Not a contour start; remember the border and keep scanning.
                prev = p;
                x += 1;
                continue;
            }
            let origin_x = x - i32::from(is_hole);
            let start_index = image.index(origin_x, y);
            let mut contour = Vec::new();
            trace(
                &mut image,
                &deltas,
                start_index,
                origin_x - 1,
                y - 1,
                is_hole,
                &mut contour,
            );
            contours.push(contour);
            // The scanner resumes with `prev = img[x]` after the trace.
            prev = image.get(image.index(x, y));
            x += 1;
        }
        y += 1;
    }

    // `cvInsertNodeIntoTree` prepends to the frame list and the final tree
    // traversal reads that list front to back, so the newest contour is first.
    contours.reverse();
    contours
}

/// `icvFetchContour` with `CHAIN_APPROX_SIMPLE`.
fn trace(
    image: &mut Mask,
    deltas: &[i32; MAX_SIZE],
    start_index: i32,
    origin_x: i32,
    origin_y: i32,
    is_hole: bool,
    contour: &mut Vec<(i32, i32)>,
) {
    let i0 = start_index;
    let mut point = (origin_x, origin_y);
    let s_end = if is_hole { 0i32 } else { 4 };
    let mut s = s_end;

    // Find the first foreground neighbour; `s_end` is the fallback direction.
    let mut i1;
    loop {
        s = (s - 1) & 7;
        i1 = i0 + deltas[s as usize];
        if image.get(i1) != 0 || s == s_end {
            break;
        }
    }

    if s == s_end {
        // A single-pixel component records its origin only.
        image.set(i0, i8::MIN | 2);
        contour.push(point);
        return;
    }

    let mut i3 = i0;
    let mut previous_direction = s ^ 4;

    loop {
        let s_end = s;
        s = s.min(MAX_SIZE as i32 - 1);
        let mut i4;
        loop {
            s += 1;
            i4 = i3 + deltas[s as usize];
            if image.get(i4) != 0 || s >= MAX_SIZE as i32 - 1 {
                break;
            }
        }
        s &= 7;

        // "right" bound: mark the pixel as belonging to a traced border.
        if ((s - 1) as u32) < (s_end as u32) {
            image.set(i3, i8::MIN | 2);
        } else if image.get(i3) == 1 {
            image.set(i3, 2);
        }

        if s != previous_direction {
            contour.push(point);
        }

        previous_direction = s;
        point.0 += CODE_DELTAS[s as usize].0;
        point.1 += CODE_DELTAS[s as usize].1;

        if i4 == i0 && i3 == i1 {
            break;
        }
        i3 = i4;
        s = (s + 4) & 7;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mask_from(rows: &[&str]) -> (Vec<u8>, usize, usize) {
        let height = rows.len();
        let width = rows[0].len();
        let mut mask = vec![0u8; width * height];
        for (y, row) in rows.iter().enumerate() {
            for (x, cell) in row.chars().enumerate() {
                mask[y * width + x] = u8::from(cell == '#');
            }
        }
        (mask, width, height)
    }

    #[test]
    fn a_single_pixel_yields_one_contour_point() {
        let (mask, width, height) = mask_from(&["...", ".#.", "..."]);
        let contours = find_contours(&mask, width, height);
        assert_eq!(contours.len(), 1);
        assert_eq!(contours[0], vec![(1, 1)]);
    }

    #[test]
    fn a_rectangle_yields_its_four_corners() {
        let (mask, width, height) = mask_from(&[".....", ".###.", ".###.", "....."]);
        let contours = find_contours(&mask, width, height);
        assert_eq!(contours.len(), 1);
        // CHAIN_APPROX_SIMPLE keeps only direction changes: the four corners.
        let mut points = contours[0].clone();
        points.sort();
        assert_eq!(points, vec![(1, 1), (1, 2), (3, 1), (3, 2)]);
    }

    #[test]
    fn separated_components_are_returned_newest_first() {
        let (mask, width, height) =
            mask_from(&["###.....", "###.....", "........", ".....###", ".....###"]);
        let contours = find_contours(&mask, width, height);
        assert_eq!(contours.len(), 2);
        // The scan finds the top-left block first; the tree prepend makes the
        // last traced contour come out first.
        assert_eq!(contours[0].iter().map(|p| p.0).min(), Some(5));
        assert_eq!(contours[1].iter().map(|p| p.0).min(), Some(0));
    }

    #[test]
    fn an_empty_mask_has_no_contours() {
        let (mask, width, height) = mask_from(&["...", "...", "..."]);
        assert!(find_contours(&mask, width, height).is_empty());
    }
}
