//! Local line ordering from `OcrEngine.cpp::Detect`, using Paddle's local row
//! sorting algorithm. This is not document layout analysis.

use std::cmp::Ordering;

/// An image-space point, matching the native floating-point box coordinates.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Point {
    pub x: f32,
    pub y: f32,
}

/// A quadrilateral whose first point is its top-left corner.
pub type LineBox = [Point; 4];

/// Sort by top-left y then x, then locally insertion-swap neighboring boxes
/// when their top-left y difference is strictly less than 10 and x is smaller.
///
/// Derived from the Paddle local row sort used in the native implementation.
/// Only the first point participates; corners are not reordered. Finite image
/// coordinates match the C++ comparisons (including equal signed zeros).
/// Nonfinite coordinates use Rust's total ordering for the initial sort.
pub fn sort_line_boxes(boxes: &mut [LineBox]) {
    boxes.sort_by(|a, b| {
        let a = a[0];
        let b = b[0];
        if a.y == b.y {
            if a.x == b.x {
                Ordering::Equal
            } else {
                a.x.total_cmp(&b.x)
            }
        } else {
            a.y.total_cmp(&b.y)
        }
    });
    for i in 1..boxes.len() {
        let mut j = i;
        while j > 0
            && (boxes[j][0].y - boxes[j - 1][0].y).abs() < 10.0
            && boxes[j][0].x < boxes[j - 1][0].x
        {
            boxes.swap(j, j - 1);
            j -= 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(x: f32, y: f32) -> LineBox {
        [
            Point { x, y },
            Point { x: x + 2.0, y },
            Point {
                x: x + 2.0,
                y: y + 2.0,
            },
            Point { x, y: y + 2.0 },
        ]
    }

    #[test]
    fn initial_order_is_y_then_x_and_corners_are_preserved() {
        let a = line(10.0, 0.0);
        let b = line(20.0, 0.0);
        let c = line(0.0, 20.0);
        let mut boxes = [c, b, a];
        sort_line_boxes(&mut boxes);
        assert_eq!(boxes, [a, b, c]);
    }

    #[test]
    fn row_threshold_is_strictly_less_than_ten() {
        for (delta, swaps) in [(9.999, true), (10.0, false), (10.001, false)] {
            let a = line(20.0, 0.0);
            let b = line(10.0, delta);
            let mut boxes = [b, a];
            sort_line_boxes(&mut boxes);
            assert_eq!(boxes, if swaps { [b, a] } else { [a, b] });
        }
    }

    #[test]
    fn insertion_rechecks_absolute_y_difference_after_each_swap() {
        let a = line(30.0, 0.0);
        let b = line(20.0, 9.0);
        let c = line(10.0, 18.0);
        let mut boxes = [c, a, b];
        sort_line_boxes(&mut boxes);
        // b first swaps past a. c then stops at a: this is a local sort,
        // not a global grouping of all boxes connected by a row threshold.
        assert_eq!(boxes, [b, a, c]);
    }

    #[test]
    fn insertion_can_move_across_multiple_neighbors() {
        let a = line(30.0, 0.0);
        let b = line(20.0, 2.0);
        let c = line(10.0, 4.0);
        let mut boxes = [a, b, c];
        sort_line_boxes(&mut boxes);
        assert_eq!(boxes, [c, b, a]);
    }

    #[test]
    fn empty_single_and_equal_x_boxes_need_no_swaps() {
        sort_line_boxes(&mut []);
        let mut single = [line(0.0, 0.0)];
        sort_line_boxes(&mut single);
        assert_eq!(single, [line(0.0, 0.0)]);
        let mut boxes = [line(0.0, 9.0), line(0.0, 0.0)];
        sort_line_boxes(&mut boxes);
        assert_eq!(boxes, [line(0.0, 0.0), line(0.0, 9.0)]);
    }
}
