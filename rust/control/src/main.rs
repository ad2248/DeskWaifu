use gpui::{
    div, px, App, AppContext, Application, Context, IntoElement, ParentElement, Render, Styled,
    Window, WindowOptions,
};

struct ControlPanel;

impl Render for ControlPanel {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .text_size(px(24.0))
            .child("DeskWaifu Control")
    }
}

fn main() {
    Application::new().run(|cx: &mut App| {
        cx.open_window(WindowOptions::default(), |_window, cx| {
            cx.new(|_cx| ControlPanel)
        })
        .expect("failed to open DeskWaifu Control window");
    });
}
