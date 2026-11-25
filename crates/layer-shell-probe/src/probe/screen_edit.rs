use super::{Crop, Size, Transform};
use crate::worker::{DragOutput, ScreenSave};

const LEFT: u8 = 1;
const RIGHT: u8 = 2;
const TOP: u8 = 4;
const BOTTOM: u8 = 8;
const DOUBLE_CLICK_MS: u32 = 400;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Target {
    Model,
    Outside,
    Move,
    Edge(u8),
}

impl Target {
    fn moves_only_model(self) -> bool {
        matches!(self, Self::Model | Self::Outside)
    }

    fn is_frame(self) -> bool {
        matches!(self, Self::Move | Self::Edge(_))
    }
}

#[derive(Clone, Copy)]
struct Frame {
    left: f64,
    top: f64,
    right: f64,
    bottom: f64,
}

impl Frame {
    fn new(crop: Crop, viewport: Size) -> Self {
        let (x, y) = crop.clip(viewport);
        Self {
            left: x.start.into(),
            top: y.start.into(),
            right: x.end.into(),
            bottom: y.end.into(),
        }
    }

    fn handle(self) -> (f64, f64, f64, f64) {
        let center = (self.left + self.right) / 2.0;
        let half = ((self.right - self.left) / 4.0).min(24.0);
        (
            center - half,
            self.top,
            center + half,
            self.bottom.min(self.top + 16.0),
        )
    }

    fn hit(self, (x, y): (f64, f64)) -> Target {
        const SLOP: f64 = 6.0;
        if x < self.left - SLOP
            || x > self.right + SLOP
            || y < self.top - SLOP
            || y > self.bottom + SLOP
        {
            return Target::Outside;
        }
        let (l, t, r, b) = self.handle();
        if x >= l && x < r && y >= t && y < b {
            return Target::Move;
        }
        let axis = |n: f64, start: f64, end: f64, first, last| {
            let a = (n - start).abs();
            let b = (n - end).abs();
            if a.min(b) > SLOP {
                0
            } else if a <= b {
                first
            } else {
                last
            }
        };
        let edges = axis(x, self.left, self.right, LEFT, RIGHT)
            | axis(y, self.top, self.bottom, TOP, BOTTOM);
        if edges != 0 {
            Target::Edge(edges)
        } else {
            Target::Model
        }
    }
}

struct Gesture<P> {
    owner: P,
    press: (f64, f64),
    start: Transform,
    target: Target,
    moved: bool,
}

pub(super) struct ScreenEdit<P> {
    pub original: Transform,
    pub pending: Option<ScreenSave>,
    pub save_error: Option<String>,
    pub cancel_on_failure: bool,
    gesture: Option<Gesture<P>>,
    frame_click: Option<(P, u32, (f64, f64))>,
}

impl<P: Clone + PartialEq> ScreenEdit<P> {
    pub fn begin(original: Transform, owner: P, press: (f64, f64), viewport: Size) -> Self {
        let mut start = original;
        if start.crop.is_none() {
            start.crop = Crop::new(0, 0, viewport.0, viewport.1);
        }
        Self {
            original,
            pending: None,
            save_error: None,
            cancel_on_failure: false,
            gesture: Some(Gesture {
                owner,
                press,
                start,
                target: Target::Model,
                moved: false,
            }),
            frame_click: None,
        }
    }

    pub fn reset_input(&mut self) {
        self.gesture = None;
        self.frame_click = None;
    }

    pub fn forget_pointer(&mut self, pointer: &P) {
        if self
            .gesture
            .as_ref()
            .is_some_and(|gesture| &gesture.owner == pointer)
        {
            self.gesture = None;
        }
        if self
            .frame_click
            .as_ref()
            .is_some_and(|(owner, _, _)| owner == pointer)
        {
            self.frame_click = None;
        }
    }

    pub fn press(&mut self, owner: &P, cursor: (f64, f64), draft: Transform, viewport: Size) {
        if self.pending.is_none() && self.gesture.is_none() {
            let target = Frame::new(draft.crop.expect("screen crop"), viewport).hit(cursor);
            self.gesture = Some(Gesture {
                owner: owner.clone(),
                press: cursor,
                start: draft,
                target,
                moved: false,
            });
        }
    }

    pub fn motion(&mut self, owner: &P, cursor: (f64, f64), viewport: Size) -> Option<Transform> {
        let gesture = self
            .gesture
            .as_mut()
            .filter(|gesture| &gesture.owner == owner)?;
        let delta = (cursor.0 - gesture.press.0, cursor.1 - gesture.press.1);
        gesture.moved |= super::placement::drag_threshold(delta);
        if !gesture.moved {
            return None;
        }
        self.frame_click = None;
        Some(drag(gesture.start, gesture.target, delta, viewport))
    }

    pub fn release(&mut self, owner: &P, cursor: (f64, f64), time: u32, viewport: Size) -> Release {
        if !self
            .gesture
            .as_ref()
            .is_some_and(|gesture| &gesture.owner == owner)
        {
            return Release::Idle;
        }
        let gesture = self.gesture.take().expect("matched gesture owner");
        if gesture.moved {
            if gesture.target.moves_only_model() {
                let placement = gesture.start.placement();
                return Release::ModelDrag(ModelDrag {
                    source: viewport,
                    zoom: placement.zoom,
                    grab: (
                        gesture.press.0 - placement.origin.0,
                        gesture.press.1 - placement.origin.1,
                    ),
                });
            }
            return Release::Idle;
        }
        let hit = Frame::new(gesture.start.crop.expect("screen crop"), viewport).hit(cursor);
        if gesture.target == Target::Outside && hit == Target::Outside {
            self.frame_click = None;
            return Release::Apply;
        }
        if gesture.target.is_frame() && hit.is_frame() {
            let double = self
                .frame_click
                .take()
                .is_some_and(|(previous, then, point)| {
                    &previous == owner
                        && time.wrapping_sub(then) <= DOUBLE_CLICK_MS
                        && !super::placement::drag_threshold((
                            cursor.0 - point.0,
                            cursor.1 - point.1,
                        ))
                });
            if !double {
                self.frame_click = Some((owner.clone(), time, cursor));
            }
            return if double {
                Release::Apply
            } else {
                Release::Idle
            };
        }
        self.frame_click = None;
        Release::Idle
    }

    pub fn finish_save(&mut self, id: u64, error: Option<String>) -> SaveOutcome {
        if self.pending.is_none_or(|pending| pending.id != id) {
            return SaveOutcome::Stale;
        }
        self.pending = None;
        if error.is_none() {
            SaveOutcome::Applied
        } else if self.cancel_on_failure {
            SaveOutcome::Canceled
        } else {
            self.save_error = error;
            SaveOutcome::Retry
        }
    }
}

#[derive(Debug, PartialEq)]
pub(super) enum Release {
    Idle,
    Apply,
    ModelDrag(ModelDrag),
}

#[derive(Debug, PartialEq)]
pub(super) struct ModelDrag {
    source: Size,
    zoom: f64,
    grab: (f64, f64),
}

impl ModelDrag {
    pub fn rebase(&self, output: &DragOutput, target: Size) -> Option<Transform> {
        let (x, y) = (
            output.pointer.0 - output.origin.0,
            output.pointer.1 - output.origin.1,
        );
        if !(0.0..f64::from(target.0)).contains(&x) || !(0.0..f64::from(target.1)).contains(&y) {
            return None;
        }
        let zoom = (self.zoom * f64::from(self.source.1) / f64::from(target.1)).clamp(0.1, 10.0);
        let factor = zoom / self.zoom;
        Transform::new(
            x - self.grab.0 * factor,
            y - self.grab.1 * factor,
            zoom,
            Crop::new(0, 0, target.0, target.1),
        )
    }
}

#[derive(Debug, PartialEq, Eq)]
pub(super) enum SaveOutcome {
    Stale,
    Applied,
    Canceled,
    Retry,
}

fn drag(start: Transform, target: Target, delta: (f64, f64), viewport: Size) -> Transform {
    let mut placement = start.placement();
    let crop = start.crop.expect("screen crop");
    let (x, y, w, h) = crop.parts();
    let crop = match target {
        Target::Model | Target::Outside => {
            placement.origin.0 += delta.0;
            placement.origin.1 += delta.1;
            placement.constrain(viewport);
            crop
        }
        Target::Move => {
            // Keep some of the crop visible and move model and crop by the same logical delta.
            let axis = |position: i32, length: u32, limit: u32, change: f64| {
                let position = i64::from(position);
                let visible = i64::from(length.min(limit).min(16));
                let min = (visible - i64::from(length)).max(i64::from(i32::MIN));
                let max = i64::from(limit - visible as u32).min(i64::from(i32::MAX));
                (position + i64::from(change.round() as i32)).clamp(min, max) - position
            };
            let dx = axis(x, w, viewport.0, delta.0);
            let dy = axis(y, h, viewport.1, delta.1);
            placement.origin.0 += dx as f64;
            placement.origin.1 += dy as f64;
            Crop::new((i64::from(x) + dx) as i32, (i64::from(y) + dy) as i32, w, h).unwrap()
        }
        Target::Edge(edges) => {
            let axis = |position: i32, length: u32, change: f64, near, far| {
                let p = i64::from(position);
                let n = i64::from(length);
                let d = i64::from(change.round() as i32);
                if edges & near != 0 {
                    let next = (p + d).clamp(
                        (p + n - i64::from(u32::MAX)).max(i64::from(i32::MIN)),
                        (p + n - 1).min(i64::from(i32::MAX)),
                    );
                    (next as i32, (p + n - next) as u32)
                } else if edges & far != 0 {
                    (position, (n + d).clamp(1, i64::from(u32::MAX)) as u32)
                } else {
                    (position, length)
                }
            };
            let (x, w) = axis(x, w, delta.0, LEFT, RIGHT);
            let (y, h) = axis(y, h, delta.1, TOP, BOTTOM);
            Crop::new(x, y, w, h).unwrap()
        }
    };
    Transform::capture(placement, Some(crop))
}

pub(super) fn paint_frame(
    canvas: &mut [u8],
    pixels: Size,
    logical: Size,
    crop: Crop,
    failed: bool,
) {
    let Frame {
        left,
        top,
        right,
        bottom,
    } = Frame::new(crop, logical);
    let mut fill = |(l, t, r, b): (f64, f64, f64, f64), color: u32| {
        let scaled = |edge: f64, size: u32, dip: u32| {
            (edge * f64::from(size) / f64::from(dip))
                .round()
                .clamp(0.0, f64::from(size)) as usize
        };
        let x = scaled(l, pixels.0, logical.0)..scaled(r, pixels.0, logical.0);
        let y = scaled(t, pixels.1, logical.1)..scaled(b, pixels.1, logical.1);
        for y in y {
            let line = y * pixels.0 as usize;
            for pixel in canvas[(line + x.start) * 4..(line + x.end) * 4]
                .as_chunks_mut::<4>()
                .0
            {
                pixel.copy_from_slice(&color.to_ne_bytes());
            }
        }
    };
    let color = if failed { 0xffff_8969 } else { 0xff51_d9ff };
    for (inset, stroke) in [(3.0, 0xff142132), (1.0, color)] {
        fill((left, top, right, bottom.min(top + inset)), stroke);
        fill((left, top.max(bottom - inset), right, bottom), stroke);
        fill((left, top, right.min(left + inset), bottom), stroke);
        fill((left.max(right - inset), top, right, bottom), stroke);
    }
    fill(Frame::new(crop, logical).handle(), color);
}
