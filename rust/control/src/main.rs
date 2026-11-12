use deskwaifu_client::{default_socket_path, request};
use gpui::{
    div, px, rgb, App, AppContext, Application, Context, InteractiveElement, IntoElement,
    ParentElement, Render, StatefulInteractiveElement, Styled, Window, WindowOptions,
};
use std::{path::PathBuf, time::Duration};

struct ControlPanel {
    socket: PathBuf,
    loading: bool,
    state: String,
    response: String,
}

impl ControlPanel {
    fn new(cx: &mut Context<Self>) -> Self {
        let mut panel = Self {
            socket: default_socket_path(),
            loading: false,
            state: "Not connected".into(),
            response: String::new(),
        };
        panel.refresh(cx);
        panel
    }

    fn refresh(&mut self, cx: &mut Context<Self>) {
        if self.loading {
            return;
        }
        self.loading = true;
        self.state = "Querying get_status…".into();
        self.response.clear();
        let path = self.socket.clone();
        // All synchronous socket I/O runs off the UI thread. No automatic retry.
        let query = cx.background_executor().spawn(async move {
            request(
                &path,
                &serde_json::json!({ "command": "get_status" }),
                Duration::from_secs(5),
            )
            .map(|reply| {
                (
                    reply.ok,
                    serde_json::to_string_pretty(&reply.value).expect("JSON value"),
                )
            })
        });
        cx.spawn(async move |panel, cx| {
            let result = query.await;
            let _ = panel.update(cx, |panel, cx| {
                panel.loading = false;
                match result {
                    Ok((ok, response)) => {
                        panel.state = if ok {
                            "Connected — get_status succeeded"
                        } else {
                            "Connected — server rejected get_status"
                        }
                        .into();
                        panel.response = response;
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
        div()
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
            .child(
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
