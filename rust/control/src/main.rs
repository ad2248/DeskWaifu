mod panel;

use deskwaifu_client::{default_socket_path, request, Response};
use gpui::{
    div, px, rgb, App, AppContext, Application, Bounds, Context, Div, InteractiveElement,
    IntoElement, MouseButton, ParentElement, Pixels, Point, Render, SharedString,
    StatefulInteractiveElement, Styled, Window, WindowOptions,
};
use panel::{ControlStatus, Status, Transform};
use serde_json::Value;
use std::{path::PathBuf, time::Duration};

const TIMEOUT: Duration = Duration::from_secs(5);
const MOUTH_STEP: f32 = 0.05;

/// One reply of a submission: either the server's answer or the transport error
/// that replaced it. A transport error never creates a `Response`.
enum Reply {
    Ok(Response),
    Failed(String),
}

impl Reply {
    fn error(&self) -> Option<String> {
        match self {
            Self::Ok(response) => panel::response_error(response),
            Self::Failed(error) => Some(format!("IPC error: {error}")),
        }
    }
}

fn log_lines(names: &[String], replies: &[Reply]) -> String {
    names
        .iter()
        .zip(replies)
        .map(|(name, reply)| match reply {
            Reply::Ok(response) => match serde_json::to_string_pretty(&response.value) {
                Ok(json) => format!("{name}:\n{json}"),
                Err(error) => format!("{name}: serialization failed: {error}"),
            },
            Reply::Failed(error) => format!("{name}: IPC error: {error}"),
        })
        .collect::<Vec<_>>()
        .join("\n\n")
}

/// The error of the submitted command itself, which is always reply zero.
fn command_error(replies: &[Reply]) -> Option<String> {
    match replies.first() {
        Some(reply) => reply.error(),
        None => Some("missing reply".into()),
    }
}

/// Applies the updates, then reports the first failure or `success`.
fn settle(errors: Vec<String>, success: impl FnOnce() -> String) -> Result<String, String> {
    match errors.into_iter().next() {
        Some(error) => Err(error),
        None => Ok(success()),
    }
}

fn read<T>(reply: Option<&Reply>, parse: fn(&Response) -> Result<T, String>) -> Result<T, String> {
    match reply {
        Some(Reply::Ok(response)) => parse(response),
        Some(Reply::Failed(error)) => Err(format!("IPC error: {error}")),
        None => Err("missing reply".into()),
    }
}

fn absorb<T>(
    slot: &mut T,
    reply: Option<&Reply>,
    parse: fn(&Response) -> Result<T, String>,
    errors: &mut Vec<String>,
) {
    if let Err(error) = panel::merge(slot, read(reply, parse)) {
        errors.push(error);
    }
}

fn absorb_controls(
    panel: &mut ControlPanel,
    replies: &[Reply],
    index: usize,
    errors: &mut Vec<String>,
) {
    let mut controls = panel.controls.clone();
    if let Err(error) = panel::merge(
        &mut controls,
        read(replies.get(index), panel::parse_control_status).map(Some),
    ) {
        errors.push(error);
        return;
    }
    if let Some(controls) = controls {
        // Show the arbitrated value the server reports, not what was typed.
        panel.mouth = controls.mouth;
        panel.look_x = controls.look_x;
        panel.look_y = controls.look_y;
        panel.controls = Some(controls);
    }
}

/// A failed status read keeps the previous status and transform, so the panel
/// never loses a known value to a transient error.
fn absorb_status(
    panel: &mut ControlPanel,
    replies: &[Reply],
    index: usize,
    errors: &mut Vec<String>,
) {
    let parsed = read(replies.get(index), panel::parse_status).map(Some);
    if let Err(error) = panel::merge(&mut panel.status, parsed) {
        errors.push(error);
        return;
    }
    if let Some(status) = panel.status.as_ref() {
        panel.transform = panel::parse_transform(status);
    }
}

type Applier = Box<dyn FnOnce(&mut ControlPanel, &[Reply]) -> Result<String, String>>;

struct ControlPanel {
    socket: PathBuf,
    loading: bool,
    state: String,
    log: String,
    models: Vec<String>,
    current_model: String,
    status: Option<Status>,
    transform: Transform,
    controls: Option<ControlStatus>,
    mouth: f32,
    look_x: f32,
    look_y: f32,
    /// Bounds of the mouth slider track, stored while painting so a click maps
    /// onto `0.0..=1.0`.
    mouth_track: Option<Bounds<Pixels>>,
}

impl ControlPanel {
    fn new(cx: &mut Context<Self>) -> Self {
        let mut panel = Self {
            socket: default_socket_path(),
            loading: false,
            state: "Not connected".into(),
            log: String::new(),
            models: Vec::new(),
            current_model: String::new(),
            status: None,
            transform: Transform::default(),
            controls: None,
            mouth: 0.0,
            look_x: 0.0,
            look_y: 0.0,
            mouth_track: None,
        };
        panel.refresh(cx);
        panel
    }

    /// The single serialized request path: every command goes through here, on
    /// the background executor, with one timeout per exchange. The `loading`
    /// guard is the only serialization; there is no queue.
    fn submit(
        &mut self,
        cx: &mut Context<Self>,
        pending: impl Into<String>,
        requests: Vec<Value>,
        apply: Applier,
    ) {
        if self.loading {
            return;
        }
        self.loading = true;
        self.state = pending.into();
        let names: Vec<String> = requests
            .iter()
            .map(|request| {
                request
                    .get("command")
                    .and_then(Value::as_str)
                    .unwrap_or("?")
                    .to_owned()
            })
            .collect();
        let path = self.socket.clone();
        // Keep every synchronous socket operation off the UI thread.
        let task = cx.background_executor().spawn(async move {
            let mut replies = Vec::with_capacity(requests.len());
            for request_value in requests {
                replies.push(match request(&path, &request_value, TIMEOUT) {
                    Ok(response) => Reply::Ok(response),
                    Err(error) => Reply::Failed(error.to_string()),
                });
            }
            replies
        });
        cx.spawn(async move |panel, cx| {
            let replies = task.await;
            let _ = panel.update(cx, |panel, cx| {
                panel.loading = false;
                panel.log = log_lines(&names, &replies);
                panel.state = match apply(panel, &replies) {
                    Ok(message) => message,
                    Err(error) => format!("Last command failed — {error}"),
                };
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn refresh(&mut self, cx: &mut Context<Self>) {
        self.submit(
            cx,
            "Querying status, models, current model, and controls…",
            vec![
                panel::status_request(),
                panel::models_request(),
                panel::current_model_request(),
                panel::control_status_request(),
            ],
            Box::new(|panel, replies| {
                let mut errors = Vec::new();
                absorb_status(panel, replies, 0, &mut errors);
                absorb(
                    &mut panel.models,
                    replies.get(1),
                    panel::parse_models,
                    &mut errors,
                );
                absorb(
                    &mut panel.current_model,
                    replies.get(2),
                    panel::parse_current_model,
                    &mut errors,
                );
                absorb_controls(panel, replies, 3, &mut errors);
                settle(errors, || "Connected — state refreshed".to_owned())
            }),
        );
    }

    /// `set_model`, `next_model`, and `prev_model` all report the new model and
    /// then re-read everything a model switch can reset, controls included.
    fn switch_model(
        &mut self,
        cx: &mut Context<Self>,
        pending: &str,
        request: Value,
        success: &'static str,
    ) {
        self.submit(
            cx,
            pending.to_owned(),
            vec![
                request,
                panel::status_request(),
                panel::current_model_request(),
                panel::control_status_request(),
            ],
            Box::new(move |panel, replies| {
                let mut errors = Vec::new();
                if let Some(error) = command_error(replies) {
                    errors.push(error);
                }
                absorb_status(panel, replies, 1, &mut errors);
                absorb(
                    &mut panel.current_model,
                    replies.get(2),
                    panel::parse_current_model,
                    &mut errors,
                );
                absorb_controls(panel, replies, 3, &mut errors);
                settle(errors, || success.to_owned())
            }),
        );
    }

    fn set_model(&mut self, name: String, cx: &mut Context<Self>) {
        self.switch_model(
            cx,
            &format!("Setting model to {name}…"),
            panel::set_model_request(&name),
            "Connected — model updated",
        );
    }

    fn cycle_model(&mut self, next: bool, cx: &mut Context<Self>) {
        self.switch_model(
            cx,
            if next {
                "Selecting the next model…"
            } else {
                "Selecting the previous model…"
            },
            panel::cycle_model_request(next),
            if next {
                "Connected — next model selected"
            } else {
                "Connected — previous model selected"
            },
        );
    }

    /// A transform write followed by a fresh `get_model_position`, so the readout
    /// is the server's value rather than the panel's guess. The follow-up read
    /// runs even when the write failed, so a rejected value reverts to the truth.
    fn submit_transform(
        &mut self,
        cx: &mut Context<Self>,
        pending: String,
        request: Value,
        success: &'static str,
    ) {
        self.submit(
            cx,
            pending,
            vec![request, panel::position_request()],
            Box::new(|panel, replies| {
                let mut errors = Vec::new();
                if let Some(error) = command_error(replies) {
                    errors.push(error);
                }
                absorb(
                    &mut panel.transform,
                    replies.get(1),
                    panel::parse_position,
                    &mut errors,
                );
                settle(errors, || success.to_owned())
            }),
        );
    }

    fn adjust_zoom(&mut self, delta: f32, cx: &mut Context<Self>) {
        match panel::zoom_step(self.transform.zoom, delta) {
            Ok(zoom) => self.submit_transform(
                cx,
                format!("Setting zoom to {}…", panel::format_number(zoom)),
                panel::zoom_request(zoom),
                "Connected — zoom updated",
            ),
            Err(error) => self.report(error, cx),
        }
    }

    fn adjust_offset(&mut self, dx: f32, dy: f32, cx: &mut Context<Self>) {
        let x = if dx == 0.0 {
            None
        } else {
            match panel::offset_step(self.transform.x, dx) {
                Ok(value) => Some(value),
                Err(error) => return self.report(error, cx),
            }
        };
        let y = if dy == 0.0 {
            None
        } else {
            match panel::offset_step(self.transform.y, dy) {
                Ok(value) => Some(value),
                Err(error) => return self.report(error, cx),
            }
        };
        let pending = match (x, y) {
            (Some(x), None) => format!("Setting x to {}…", panel::format_number(x)),
            (None, Some(y)) => format!("Setting y to {}…", panel::format_number(y)),
            (Some(x), Some(y)) => format!(
                "Setting position to ({}, {})…",
                panel::format_number(x),
                panel::format_number(y)
            ),
            (None, None) => return,
        };
        self.submit_transform(
            cx,
            pending,
            panel::position_set_request(x, y),
            "Connected — position updated",
        );
    }

    fn toggle_hidden(&mut self, cx: &mut Context<Self>) {
        self.submit(
            cx,
            "Toggling visibility…",
            vec![panel::toggle_hidden_request(), panel::status_request()],
            Box::new(|panel, replies| {
                let mut errors = Vec::new();
                let hidden = match read(replies.first(), panel::parse_hidden) {
                    Ok(hidden) => Some(hidden),
                    Err(error) => {
                        errors.push(error);
                        None
                    }
                };
                absorb_status(panel, replies, 1, &mut errors);
                // The refreshed status is authoritative; the toggle reply alone
                // could contradict it.
                let shown = panel.status.as_ref().map(|status| status.hidden).or(hidden);
                settle(errors, || match shown {
                    Some(true) => "Connected — model is now hidden".to_owned(),
                    Some(false) => "Connected — model is now visible".to_owned(),
                    None => "Connected — visibility toggled".to_owned(),
                })
            }),
        );
    }

    /// Control writes always follow with `get_control_status`, even when the
    /// write failed, so the readout stays the arbitrated truth.
    fn submit_control(
        &mut self,
        cx: &mut Context<Self>,
        pending: String,
        request: Value,
        success: &'static str,
    ) {
        self.submit(
            cx,
            pending,
            vec![request, panel::control_status_request()],
            Box::new(|panel, replies| {
                let mut errors = Vec::new();
                if let Some(error) = command_error(replies) {
                    errors.push(error);
                }
                absorb_controls(panel, replies, 1, &mut errors);
                settle(errors, || success.to_owned())
            }),
        );
    }

    fn set_mouth(&mut self, value: f32, cx: &mut Context<Self>) {
        let mouth = match panel::parse_mouth(&panel::format_number(value)) {
            Ok(value) => value,
            Err(error) => return self.report(error, cx),
        };
        self.mouth = mouth;
        self.submit_control(
            cx,
            format!("Setting mouth to {}…", panel::format_number(mouth)),
            panel::mouth_request(mouth),
            "Connected — mouth applied",
        );
    }

    fn set_mouth_at(&mut self, position: Point<Pixels>, cx: &mut Context<Self>) {
        let Some(track) = self.mouth_track else {
            return;
        };
        let width = track.size.width;
        if width <= px(0.0) {
            return;
        }
        let fraction = ((position.x - track.origin.x) / width).clamp(0.0, 1.0);
        self.set_mouth(fraction, cx);
    }

    fn nudge_look(&mut self, dx: f32, dy: f32, cx: &mut Context<Self>) {
        let x = match panel::look_step(self.look_x, dx) {
            Ok(value) => value,
            Err(error) => return self.report(error, cx),
        };
        let y = match panel::look_step(self.look_y, dy) {
            Ok(value) => value,
            Err(error) => return self.report(error, cx),
        };
        self.look_x = x;
        self.look_y = y;
        self.apply_look(cx);
    }

    fn apply_look(&mut self, cx: &mut Context<Self>) {
        let (x, y) = (self.look_x, self.look_y);
        self.submit_control(
            cx,
            format!(
                "Setting look to ({}, {})…",
                panel::format_number(x),
                panel::format_number(y)
            ),
            panel::look_request(x, y),
            "Connected — look applied",
        );
    }

    fn release_controls(&mut self, cx: &mut Context<Self>) {
        self.submit_control(
            cx,
            "Releasing external controls…".to_owned(),
            panel::release_request(),
            "Connected — external controls released",
        );
    }

    /// Clears only the look channel, returning the model to mouse following while
    /// any mouth value stays as it is.
    fn reset_look(&mut self, cx: &mut Context<Self>) {
        self.submit_control(
            cx,
            "Resetting look to mouse following…".to_owned(),
            panel::look_reset_request(),
            "Connected — look reset",
        );
    }

    /// Local validation failures never reach the socket and never overwrite the
    /// last known server state.
    fn report(&mut self, error: String, cx: &mut Context<Self>) {
        self.state = format!("Input rejected — {error}");
        cx.notify();
    }
}

fn action_button(
    cx: &Context<ControlPanel>,
    id: &'static str,
    label: impl Into<SharedString>,
    enabled: bool,
    handler: impl Fn(&mut ControlPanel, &mut Context<ControlPanel>) + 'static,
) -> impl IntoElement {
    div()
        .id(id)
        .px_3()
        .py_2()
        .bg(rgb(if enabled { 0x315d85 } else { 0x454545 }))
        .child(label.into())
        .on_click(cx.listener(move |panel, _, _, cx| {
            if enabled {
                handler(panel, cx);
            }
        }))
}

fn label_row(caption: &str, value: String) -> Div {
    div()
        .flex()
        .flex_row()
        .gap_2()
        .child(
            div()
                .w(px(160.0))
                .text_color(rgb(0x9aa4b0))
                .child(caption.to_owned()),
        )
        .child(div().child(value))
}

/// A caption, the current value, and ± steppers that both call `step` with a
/// `-1.0` or `+1.0` direction.
fn number_row(
    cx: &Context<ControlPanel>,
    caption: &'static str,
    value: f32,
    dec_id: &'static str,
    inc_id: &'static str,
    enabled: bool,
    step: impl Fn(&mut ControlPanel, f32, &mut Context<ControlPanel>) + Clone + 'static,
) -> impl IntoElement {
    let down = step.clone();
    div()
        .flex()
        .flex_row()
        .items_center()
        .gap_2()
        .child(
            div()
                .w(px(160.0))
                .text_color(rgb(0x9aa4b0))
                .child(caption.to_owned()),
        )
        .child(div().w(px(64.0)).child(panel::format_number(value)))
        .child(action_button(
            cx,
            dec_id,
            "−",
            enabled,
            move |panel, cx| down(panel, -1.0, cx),
        ))
        .child(action_button(cx, inc_id, "+", enabled, move |panel, cx| {
            step(panel, 1.0, cx)
        }))
}

fn section(title: &str) -> impl IntoElement {
    div().text_size(px(18.0)).child(title.to_owned())
}

impl Render for ControlPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let ready = !self.loading;
        let mut content = div()
            .id("control-panel")
            .size_full()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap_3()
            .p_4()
            .bg(rgb(0x20242a))
            .text_color(rgb(0xf0f0f0))
            .child(div().text_size(px(24.0)).child("DeskWaifu Control"));

        // Connection.
        content = content
            .child(section("Connection"))
            .child(label_row("Socket:", self.socket.display().to_string()))
            .child(label_row("State:", self.state.clone()))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap_2()
                    .child(action_button(
                        cx,
                        "refresh",
                        if self.loading {
                            "Loading…"
                        } else {
                            "Refresh"
                        },
                        ready,
                        |panel, cx| panel.refresh(cx),
                    ))
                    .child(div().text_color(rgb(0x9aa4b0)).child(
                        "Refresh re-reads status, models, current model, and control status.",
                    )),
            );

        // Status.
        content = content.child(section("Status"));
        content = match self.status.clone() {
            Some(status) => content
                .child(label_row("Model:", status.model))
                .child(label_row(
                    "Model index:",
                    format!("{} of {}", status.model_index, status.model_count),
                ))
                .child(label_row(
                    "Hidden:",
                    if status.hidden { "yes" } else { "no" }.to_owned(),
                ))
                .child(label_row("Zoom:", panel::format_number(status.zoom)))
                .child(label_row("X:", panel::format_number(status.x)))
                .child(label_row("Y:", panel::format_number(status.y)))
                .child(label_row(
                    "Renderer available:",
                    if status.renderer_available {
                        "yes".to_owned()
                    } else {
                        "no — the Rust runtime serves no renderer yet".to_owned()
                    },
                )),
            None => content.child(label_row("Status:", "unknown — not read yet".to_owned())),
        };

        // Models.
        content = content.child(section("Models")).child(
            div()
                .flex()
                .flex_row()
                .gap_2()
                .child(action_button(
                    cx,
                    "prev-model",
                    "Previous",
                    ready,
                    |panel, cx| panel.cycle_model(false, cx),
                ))
                .child(action_button(
                    cx,
                    "next-model",
                    "Next",
                    ready,
                    |panel, cx| panel.cycle_model(true, cx),
                )),
        );
        if self.models.is_empty() {
            content = content.child(label_row("Available models:", "none read yet".to_owned()));
        }
        for model in self.models.clone() {
            let selected = model == self.current_model;
            let label = if selected {
                format!("{model} (current)")
            } else {
                model.clone()
            };
            content = content.child(
                div()
                    .id(SharedString::from(format!("model-{model}")))
                    .px_3()
                    .py_2()
                    .bg(rgb(if !ready || selected {
                        0x454545
                    } else {
                        0x315d85
                    }))
                    .child(label)
                    .on_click(cx.listener(move |panel, _, _, cx| {
                        if !selected && !panel.loading {
                            panel.set_model(model.clone(), cx);
                        }
                    })),
            );
        }

        // Transform.
        content = content
            .child(section("Transform"))
            .child(number_row(
                cx,
                "Zoom (0.1–10.0)",
                self.transform.zoom,
                "zoom-out",
                "zoom-in",
                ready,
                |panel, direction, cx| panel.adjust_zoom(direction * panel::STEP, cx),
            ))
            .child(number_row(
                cx,
                "X offset",
                self.transform.x,
                "x-dec",
                "x-inc",
                ready,
                |panel, direction, cx| panel.adjust_offset(direction * panel::STEP, 0.0, cx),
            ))
            .child(number_row(
                cx,
                "Y offset",
                self.transform.y,
                "y-dec",
                "y-inc",
                ready,
                |panel, direction, cx| panel.adjust_offset(0.0, direction * panel::STEP, cx),
            ));

        // Visibility.
        content = content.child(section("Visibility")).child(
            div()
                .flex()
                .flex_row()
                .items_center()
                .gap_2()
                .child(action_button(
                    cx,
                    "toggle-hidden",
                    "Show / hide",
                    ready,
                    |panel, cx| panel.toggle_hidden(cx),
                ))
                .child(
                    div().child(match self.status.as_ref().map(|status| status.hidden) {
                        Some(true) => "hidden: yes".to_owned(),
                        Some(false) => "hidden: no".to_owned(),
                        None => "hidden: unknown".to_owned(),
                    }),
                ),
        );

        // External controls.
        let mouth = self.mouth.clamp(0.0, 1.0);
        content = content
            .child(section("External controls"))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap_2()
                    .child(
                        div()
                            .w(px(160.0))
                            .text_color(rgb(0x9aa4b0))
                            .child("Mouth (0.0–1.0)"),
                    )
                    .child(div().w(px(64.0)).child(panel::format_number(self.mouth)))
                    .child(action_button(cx, "mouth-dec", "−", ready, |panel, cx| {
                        let value = panel.mouth - MOUTH_STEP;
                        panel.set_mouth(value, cx)
                    }))
                    .child(action_button(cx, "mouth-inc", "+", ready, |panel, cx| {
                        let value = panel.mouth + MOUTH_STEP;
                        panel.set_mouth(value, cx)
                    })),
            )
            .child(
                div()
                    .w_full()
                    .h(px(18.0))
                    .child(
                        div()
                            .size_full()
                            .bg(rgb(0x454545))
                            .child(div().h_full().w(gpui::relative(mouth)).bg(rgb(0x5aa0e0))),
                    )
                    .on_children_prepainted({
                        let panel = cx.entity().downgrade();
                        move |bounds: Vec<Bounds<Pixels>>, _, cx: &mut App| {
                            if let Some(track) = bounds.first() {
                                let _ = panel.update(cx, |panel, _| {
                                    panel.mouth_track = Some(*track);
                                });
                            }
                        }
                    })
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(|panel, event: &gpui::MouseDownEvent, _, cx| {
                            if !panel.loading {
                                panel.set_mouth_at(event.position, cx);
                            }
                        }),
                    ),
            )
            .child(number_row(
                cx,
                "Look X (−1 to 1)",
                self.look_x,
                "look-x-dec",
                "look-x-inc",
                ready,
                |panel, direction, cx| panel.nudge_look(direction * panel::STEP, 0.0, cx),
            ))
            .child(number_row(
                cx,
                "Look Y (−1 to 1)",
                self.look_y,
                "look-y-dec",
                "look-y-inc",
                ready,
                |panel, direction, cx| panel.nudge_look(0.0, direction * panel::STEP, cx),
            ))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .gap_2()
                    .child(action_button(
                        cx,
                        "look-apply",
                        "Apply look",
                        ready,
                        |panel, cx| panel.apply_look(cx),
                    ))
                    .child(action_button(
                        cx,
                        "look-reset",
                        "Reset look",
                        ready,
                        |panel, cx| panel.reset_look(cx),
                    ))
                    .child(action_button(
                        cx,
                        "release-controls",
                        "Release source",
                        ready,
                        |panel, cx| panel.release_controls(cx),
                    )),
            );

        content = content.child(match self.controls.as_ref() {
            Some(controls) => div()
                .flex()
                .flex_col()
                .gap_1()
                .child(label_row(
                    "has_mouth:",
                    format!(
                        "{} ({})",
                        controls.has_mouth,
                        panel::format_number(controls.mouth)
                    ),
                ))
                .child(label_row(
                    "has_look:",
                    format!(
                        "{} ({}, {})",
                        controls.has_look,
                        panel::format_number(controls.look_x),
                        panel::format_number(controls.look_y)
                    ),
                )),
            None => label_row("Control status:", "unknown — not read yet".to_owned()),
        });

        // The runtime has no surface for these, so they are stated, not faked.
        content = content
            .child(section("Not available yet"))
            .child(div().text_color(rgb(0x9aa4b0)).child(
            "The Rust runtime serves no expressions, motions, hotkeys, OCR, voice input, skins, \
                 outputs, or transform editing yet, so this panel has no controls for them.",
        ));

        // Response log: only the last submission.
        content = content.child(section("Last exchange")).child(
            div()
                .id("response-log")
                .h(px(200.0))
                .w_full()
                .overflow_y_scroll()
                .bg(rgb(0x181b1f))
                .p_2()
                .child(if self.log.is_empty() {
                    "No exchange yet.".to_owned()
                } else {
                    self.log.clone()
                }),
        );

        content
    }
}

fn main() {
    Application::new().run(|cx: &mut App| {
        cx.open_window(WindowOptions::default(), |_window, cx| {
            cx.new(ControlPanel::new)
        })
        .expect("failed to open DeskWaifu Control window");
        cx.activate(true);
    });
}
