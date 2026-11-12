use deskwaifu_client::{default_socket_path, request};
use gpui::{
    div, px, rgb, App, AppContext, Application, Context, InteractiveElement, IntoElement,
    ParentElement, Render, SharedString, StatefulInteractiveElement, Styled, Window, WindowOptions,
};
use std::{path::PathBuf, time::Duration};

struct ControlPanel {
    socket: PathBuf,
    loading: bool,
    state: String,
    response: String,
    models: Vec<String>,
    current_model: String,
}

impl ControlPanel {
    fn new(cx: &mut Context<Self>) -> Self {
        let mut panel = Self {
            socket: default_socket_path(),
            loading: false,
            state: "Not connected".into(),
            response: String::new(),
            models: Vec::new(),
            current_model: String::new(),
        };
        panel.refresh(cx);
        panel
    }

    fn refresh(&mut self, cx: &mut Context<Self>) {
        if self.loading {
            return;
        }
        self.loading = true;
        self.state = "Querying get_status, models, and current model…".into();
        self.response.clear();
        let path = self.socket.clone();
        // Keep every synchronous socket operation off the UI thread and serialize requests.
        let query = cx.background_executor().spawn(async move {
            let status = request(
                &path,
                &serde_json::json!({ "command": "get_status" }),
                Duration::from_secs(5),
            )?;
            let models = request(
                &path,
                &serde_json::json!({ "command": "get_available_models" }),
                Duration::from_secs(5),
            )?;
            let current = request(
                &path,
                &serde_json::json!({ "command": "get_current_model" }),
                Duration::from_secs(5),
            )?;
            Ok::<_, std::io::Error>((status, models, current))
        });
        cx.spawn(async move |panel, cx| {
            let result = query.await;
            let _ = panel.update(cx, |panel, cx| {
                panel.loading = false;
                match result {
                    Ok((status, models, current)) => {
                        panel.state = if status.ok && models.ok && current.ok {
                            "Connected — status and model queries succeeded"
                        } else {
                            "Connected — server rejected one or more queries"
                        }
                        .into();
                        panel.response =
                            serde_json::to_string_pretty(&status.value).expect("JSON value");
                        if models.ok {
                            panel.models = models
                                .value
                                .get("models")
                                .and_then(|value| value.as_array())
                                .map(|values| {
                                    values
                                        .iter()
                                        .filter_map(|value| value.as_str().map(str::to_owned))
                                        .collect()
                                })
                                .unwrap_or_default();
                        }
                        if current.ok {
                            panel.current_model = current
                                .value
                                .get("model")
                                .and_then(|value| value.as_str())
                                .unwrap_or_default()
                                .to_owned();
                        }
                        for (name, reply) in [
                            ("get_available_models", models),
                            ("get_current_model", current),
                        ] {
                            if !reply.ok {
                                panel.response.push_str(&format!(
                                    "\n\n{name} failed: {}",
                                    serde_json::to_string_pretty(&reply.value).expect("JSON value")
                                ));
                            }
                        }
                    }
                    Err(error) => {
                        panel.state = "Disconnected / IPC error".into();
                        panel.response = error.to_string();
                    }
                }
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn set_model(&mut self, name: String, cx: &mut Context<Self>) {
        if self.loading {
            return;
        }
        self.loading = true;
        self.state = format!("Setting model to {name}…");
        self.response.clear();
        let path = self.socket.clone();
        let query = cx.background_executor().spawn(async move {
            let reply = request(
                &path,
                &serde_json::json!({ "command": "set_model", "name": name }),
                Duration::from_secs(5),
            )?;
            let status = request(
                &path,
                &serde_json::json!({ "command": "get_status" }),
                Duration::from_secs(5),
            )?;
            let current = request(
                &path,
                &serde_json::json!({ "command": "get_current_model" }),
                Duration::from_secs(5),
            )?;
            Ok::<_, std::io::Error>((reply, status, current))
        });
        cx.spawn(async move |panel, cx| {
            let result = query.await;
            let _ = panel.update(cx, |panel, cx| {
                panel.loading = false;
                match result {
                    Ok((reply, status, current)) => {
                        panel.state = if reply.ok && status.ok && current.ok {
                            "Connected — model updated"
                        } else {
                            "Connected — model change or refresh failed"
                        }
                        .into();
                        panel.response = format!(
                            "set_model:\n{}\n\nget_status:\n{}",
                            serde_json::to_string_pretty(&reply.value).expect("JSON value"),
                            serde_json::to_string_pretty(&status.value).expect("JSON value")
                        );
                        if status.ok {
                            panel.response.push_str(&format!(
                                "\n\nget_current_model:\n{}",
                                serde_json::to_string_pretty(&current.value).expect("JSON value")
                            ));
                        }
                        if current.ok {
                            panel.current_model = current
                                .value
                                .get("model")
                                .and_then(|value| value.as_str())
                                .unwrap_or_default()
                                .to_owned();
                        }
                    }
                    Err(error) => {
                        panel.state = "Disconnected / IPC error".into();
                        panel.response = error.to_string();
                    }
                }
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }
}

impl Render for ControlPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut content = div()
            .size_full()
            .flex()
            .flex_col()
            .gap_3()
            .p_4()
            .bg(rgb(0x20242a))
            .text_color(rgb(0xf0f0f0))
            .child(div().text_size(px(24.0)).child("DeskWaifu Status"))
            .child(div().child(format!("Socket: {}", self.socket.display())))
            .child(div().child(self.state.clone()))
            .child(
                div()
                    .id("refresh-status")
                    .px_3()
                    .py_2()
                    .bg(rgb(if self.loading { 0x454545 } else { 0x315d85 }))
                    .child(if self.loading {
                        "Loading…"
                    } else {
                        "Refresh status"
                    })
                    .on_click(cx.listener(|panel, _, _, cx| panel.refresh(cx))),
            )
            .child(div().text_size(px(18.0)).child("Available models"));

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
                    .bg(rgb(if self.loading || selected {
                        0x454545
                    } else {
                        0x315d85
                    }))
                    .child(label)
                    .on_click(cx.listener(move |panel, _, _, cx| {
                        if !selected {
                            panel.set_model(model.clone(), cx);
                        }
                    })),
            );
        }

        content.child(
            div()
                .id("status-response")
                .flex_1()
                .min_h_0()
                .overflow_y_scroll()
                .child(self.response.clone()),
        )
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
