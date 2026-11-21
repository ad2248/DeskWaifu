mod bindings;
mod edit;
mod ipc;

use gpui_kit::component::{
    Disableable,
    button::{Button, ButtonVariants},
    input::{Input, InputState},
};
use gpui_kit::prelude::FluentBuilder;
use gpui_kit::*;
use ipc::{AcceptedOperation, OperationStatus, PanelSection, PanelTabs, PanelView};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

const PAPER: u32 = 0xf5f3f8;
const INK: u32 = 0x272235;
const MUTED: u32 = 0x605971;
const MAX_BINDING_QUERIES: usize = 32;
const BINDING_SCAN_BUDGET: Duration = Duration::from_secs(4);

struct Panel {
    socket: PathBuf,
    focus: FocusHandle,
    tabs: Vec<ipc::PanelTab>,
    selected: usize,
    input: Option<Entity<InputState>>,
    dynamic: Option<PanelView>,
    page_bindings: Vec<Option<bindings::PageBindings>>,
    binding_scan: Option<Task<()>>,
    edit: Option<edit::EditSession>,
    busy: bool,
    tabs_error: Option<String>,
    query_error: Option<String>,
    notice: Option<String>,
}

impl Panel {
    fn controls_locked(&self) -> bool {
        self.busy || self.edit.is_some()
    }

    fn reconnect(&mut self, window: &Window, cx: &mut Context<Self>) {
        if self.controls_locked() {
            return;
        }
        self.binding_scan = None;
        self.page_bindings.clear();
        self.busy = true;
        let socket = self.socket.clone();
        let response = cx.background_spawn(async move {
            ipc::json_command::<PanelTabs>(&socket, &["panel-tabs".into()])
        });
        cx.spawn_in(window, async move |view, cx| {
            let response = response.await;
            let _ = view.update_in(cx, |this, window, cx| {
                this.busy = false;
                this.notice = None;
                match response {
                    Ok(response) => {
                        let old = this
                            .tabs
                            .get(this.selected)
                            .map(|tab| (&tab.plugin, &tab.id));
                        let index = response
                            .tabs
                            .iter()
                            .position(|tab| old == Some((&tab.plugin, &tab.id)));
                        let input = index
                            .and_then(|_| this.input.as_ref())
                            .map(|input| input.read(cx).value().to_string());
                        this.tabs = response.tabs;
                        this.page_bindings.resize_with(this.tabs.len(), || None);
                        this.tabs_error = None;
                        this.select_tab(index.unwrap_or(0), input, window, cx);
                        this.discover_bindings(cx);
                    }
                    Err(error) => {
                        this.tabs.clear();
                        this.input = None;
                        this.dynamic = None;
                        this.query_error = None;
                        this.tabs_error = Some(error);
                    }
                }
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn binding_query(&self, index: usize) -> Option<(usize, String, Vec<String>)> {
        let tab = self.tabs.get(index)?;
        let args = &tab.query.as_ref()?.args;
        (args.first() == Some(&tab.plugin)).then(|| (index, tab.plugin.clone(), args.clone()))
    }

    fn discover_bindings(&mut self, cx: &mut Context<Self>) {
        let queries: Vec<_> = (0..self.tabs.len())
            .filter(|index| *index != self.selected)
            .filter_map(|index| self.binding_query(index))
            .take(MAX_BINDING_QUERIES)
            .collect();
        let socket = self.socket.clone();
        // Keep the scan independent of the selected page and issue only one scan request at a time.
        self.binding_scan = Some(cx.spawn(async move |panel, cx| {
            let deadline = Instant::now() + BINDING_SCAN_BUDGET;
            for (index, plugin, args) in queries {
                if Instant::now() >= deadline {
                    break;
                }
                let socket = socket.clone();
                let page = cx
                    .background_spawn(async move {
                        ipc::json_command::<PanelView>(&socket, &args)
                            .map(|view| bindings::PageBindings::from_view(&view, &plugin))
                    })
                    .await;
                if let Ok(page) = page
                    && panel
                        .update(cx, |panel, _| {
                            // Foreground page loads take precedence over discovery with default input.
                            if let Some(slot @ None) = panel.page_bindings.get_mut(index) {
                                *slot = Some(page);
                            }
                        })
                        .is_err()
                {
                    break;
                }
            }
        }));
    }

    fn select_tab(
        &mut self,
        index: usize,
        input: Option<String>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.selected = index;
        self.dynamic = None;
        self.query_error = None;
        self.notice = None;
        self.input = self
            .tabs
            .get(index)
            .and_then(|tab| tab.query.as_ref())
            .and_then(|query| query.input_placeholder.clone())
            .map(|placeholder| {
                cx.new(|cx| {
                    InputState::new(window, cx)
                        .placeholder(placeholder)
                        .default_value(input.unwrap_or_default())
                })
            });
        window.focus(&self.focus, cx);
        self.query(cx);
        cx.notify();
    }

    fn query_args(&self, cx: &App) -> Option<Vec<String>> {
        let query = self.tabs.get(self.selected)?.query.as_ref()?;
        let input = self
            .input
            .as_ref()
            .map(|input| input.read(cx).value())
            .unwrap_or_default();
        Some(query.request_args(&input))
    }

    fn query(&mut self, cx: &mut Context<Self>) {
        if self.controls_locked() {
            return;
        }
        let Some(args) = self.query_args(cx) else {
            return;
        };
        self.busy = true;
        let socket = self.socket.clone();
        let response =
            cx.background_spawn(async move { ipc::json_command::<PanelView>(&socket, &args) });
        cx.spawn(async move |view, cx| {
            let response = response.await;
            let _ = view.update(cx, |this, cx| {
                this.set_dynamic(response);
                this.busy = false;
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn set_dynamic(&mut self, response: Result<PanelView, String>) {
        if let (Some(tab), Some(slot)) = (
            self.tabs.get(self.selected),
            self.page_bindings.get_mut(self.selected),
        ) {
            *slot = Some(
                response
                    .as_ref()
                    .map(|view| bindings::PageBindings::from_view(view, &tab.plugin))
                    .unwrap_or_default(),
            );
        }
        match response {
            Ok(view) => {
                self.dynamic = Some(view);
                self.query_error = None;
            }
            Err(error) => {
                self.dynamic = None;
                self.query_error = Some(error);
            }
        }
    }

    fn execute(&mut self, args: Vec<String>, cx: &mut Context<Self>) {
        self.execute_from(args, None, cx);
    }

    fn execute_from(&mut self, args: Vec<String>, source: Option<usize>, cx: &mut Context<Self>) {
        if self.controls_locked() || args.is_empty() {
            return;
        }
        self.busy = true;
        self.notice = Some("正在执行…".into());
        let socket = self.socket.clone();
        let query_args = self.query_args(cx);
        let completion = match source {
            Some(index) => self
                .page_bindings
                .get(index)
                .and_then(Option::as_ref)
                .and_then(|page| page.completion.clone()),
            None => self
                .dynamic
                .as_ref()
                .and_then(|view| view.completion_query.clone()),
        };
        let source_query = source
            .filter(|index| *index != self.selected)
            .and_then(|index| self.binding_query(index));
        let submit_socket = socket.clone();
        let submitted = cx.background_spawn(async move { ipc::command(&submit_socket, &args) });
        cx.spawn(async move |view, cx| {
            let notice = match submitted.await {
                Ok(output) => match serde_json::from_str::<AcceptedOperation>(&output) {
                    Ok(operation) => {
                        let id = operation.accepted_operation_id;
                        if let Some(completion) = completion {
                            if view
                                .update(cx, |this, cx| {
                                    this.notice = Some(format!("已受理操作 #{id}，等待完成…"));
                                    cx.notify();
                                })
                                .is_err()
                            {
                                return;
                            }
                            let deadline = std::time::Instant::now() + Duration::from_secs(6);
                            loop {
                                if std::time::Instant::now() >= deadline {
                                    break format!("尚未确认操作 #{id} 的结果");
                                }
                                cx.background_executor()
                                    .timer(Duration::from_millis(200))
                                    .await;
                                let status_socket = socket.clone();
                                let completion = completion.clone();
                                let status = cx
                                    .background_spawn(async move {
                                        ipc::json_command::<OperationStatus>(
                                            &status_socket,
                                            &completion,
                                        )
                                    })
                                    .await;
                                match status {
                                    Ok(status) => {
                                        if let Some(operation) = status
                                            .snapshot
                                            .completed
                                            .into_iter()
                                            .find(|operation| operation.id == id)
                                        {
                                            break match operation.error {
                                                Some(error) => format!("操作 #{id} 失败：{error}"),
                                                None => format!("操作 #{id} 已完成"),
                                            };
                                        }
                                    }
                                    Err(error) => {
                                        break format!("无法确认操作 #{id} 的结果：{error}");
                                    }
                                }
                                if view.upgrade().is_none() {
                                    return;
                                }
                            }
                        } else {
                            format!("已受理操作 #{id}")
                        }
                    }
                    Err(_) if output.is_empty() => "操作已响应".into(),
                    Err(_) => output,
                },
                Err(error) => format!("请求失败：{error}"),
            };
            let (response, source_page) = cx
                .background_spawn(async move {
                    let response =
                        query_args.map(|args| ipc::json_command::<PanelView>(&socket, &args));
                    let source_page = source_query.map(|(index, plugin, args)| {
                        let page = ipc::json_command::<PanelView>(&socket, &args)
                            .map(|view| bindings::PageBindings::from_view(&view, &plugin))
                            .unwrap_or_default();
                        (index, page)
                    });
                    (response, source_page)
                })
                .await;
            let _ = view.update(cx, |this, cx| {
                if let Some(response) = response {
                    this.set_dynamic(response);
                }
                if let Some((index, page)) = source_page
                    && let Some(slot) = this.page_bindings.get_mut(index)
                {
                    *slot = Some(page);
                }
                this.notice = Some(notice);
                this.busy = false;
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn sections(&self) -> impl Iterator<Item = &PanelSection> {
        self.tabs
            .get(self.selected)
            .into_iter()
            .flat_map(|tab| &tab.sections)
            .chain(self.dynamic.iter().flat_map(|view| &view.sections))
    }

    fn render_section(&self, index: usize, section: &PanelSection, cx: &mut Context<Self>) -> Div {
        let actions = section
            .actions
            .iter()
            .enumerate()
            .map(|(action_index, action)| {
                let args = action.args.clone();
                let id = SharedString::from(format!("action-{index}-{action_index}"));
                Button::new(ElementId::Name(id))
                    .label(action.label.clone())
                    .when(action.selected, |button| button.primary())
                    .when(!action.selected, |button| button.outline())
                    .disabled(self.controls_locked() || args.is_empty())
                    .on_click(cx.listener(move |this, _, _, cx| this.execute(args.clone(), cx)))
            });
        section_box(section.title.clone())
            .children(section.lines.iter().cloned().map(secondary))
            .when(!section.actions.is_empty(), |section_box| {
                section_box.child(
                    div()
                        .flex()
                        .gap_2()
                        .when(section.cycle, |row| row.flex_col())
                        .when(!section.cycle, |row| row.flex_wrap())
                        .children(actions),
                )
            })
    }
}

fn section_box(title: impl Into<SharedString>) -> Div {
    div()
        .flex()
        .flex_col()
        .gap_3()
        .p_4()
        .rounded_lg()
        .bg(rgb(0xffffff))
        .border_1()
        .border_color(rgb(0xe4dcec))
        .child(div().font_weight(FontWeight::SEMIBOLD).child(title.into()))
}

fn secondary(text: impl Into<SharedString>) -> Div {
    div().text_sm().text_color(rgb(MUTED)).child(text.into())
}

impl Render for Panel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let tab_bar = div()
            .flex()
            .flex_wrap()
            .gap_2()
            .children(self.tabs.iter().enumerate().map(|(index, tab)| {
                Button::new(("tab", index))
                    .label(tab.title.clone())
                    .when(index == self.selected, |button| button.primary())
                    .disabled(self.controls_locked())
                    .on_click(cx.listener(move |this, _, window, cx| {
                        if index != this.selected {
                            this.select_tab(index, None, window, cx);
                        }
                    }))
            }));
        let can_cycle = self.edit.is_none()
            && self
                .sections()
                .any(|section| section.cycle && !section.actions.is_empty());
        let sections: Vec<_> = self
            .sections()
            .enumerate()
            .map(|(index, section)| self.render_section(index, section, cx))
            .collect();
        let query = self
            .tabs
            .get(self.selected)
            .and_then(|tab| tab.query.as_ref())
            .map(|query| {
                let input = self
                    .input
                    .as_ref()
                    .zip(query.input_placeholder.as_ref())
                    .map(|(input, label)| {
                        div().flex_1().child(
                            Input::new(input)
                                .aria_label(label.clone())
                                .disabled(self.controls_locked()),
                        )
                    });
                div().flex().items_center().gap_2().children(input).child(
                    Button::new("refresh-tab")
                        .primary()
                        .label("刷新")
                        .disabled(self.controls_locked())
                        .on_click(cx.listener(|this, _, _, cx| this.query(cx))),
                )
            });

        let edit = self.render_edit(cx);
        div()
            .id("panel")
            .size_full()
            .overflow_y_scroll()
            .bg(rgb(PAPER))
            .text_color(rgb(INK))
            .font_family("Noto Sans CJK SC")
            .p_6()
            .flex()
            .flex_col()
            .gap_4()
            .track_focus(&self.focus)
            .when(can_cycle, |div| div.key_context("DeskWaifuCycle"))
            .capture_key_down(cx.listener(|this, event: &KeyDownEvent, window, cx| {
                let modifiers = event.keystroke.modifiers;
                if this.edit.is_some()
                    || this
                        .input
                        .as_ref()
                        .is_some_and(|input| input.read(cx).focus_handle(cx).is_focused(window))
                {
                    return;
                }
                if event.keystroke.key == "tab" {
                    if modifiers.control
                        || modifiers.alt
                        || modifiers.platform
                        || modifiers.function
                    {
                        return;
                    }
                    let args = this
                        .sections()
                        .find_map(|section| section.cycle_action(modifiers.shift))
                        .map(|action| action.args.clone());
                    if let Some(args) = args {
                        cx.stop_propagation();
                        if !event.is_held {
                            this.execute(args, cx);
                        }
                    }
                } else if let Some((source, args)) =
                    bindings::resolve(&this.page_bindings, this.selected, event)
                {
                    cx.stop_propagation();
                    if !event.is_held {
                        this.execute_from(args.to_vec(), Some(source), cx);
                    }
                }
            }))
            .child(
                div()
                    .flex()
                    .justify_between()
                    .items_center()
                    .child(
                        div()
                            .text_2xl()
                            .font_weight(FontWeight::BOLD)
                            .child("DeskWaifu 控制面板"),
                    )
                    .child(
                        Button::new("refresh-tabs")
                            .label("重新连接")
                            .disabled(self.controls_locked())
                            .on_click(
                                cx.listener(|this, _, window, cx| this.reconnect(window, cx)),
                            ),
                    ),
            )
            .child(tab_bar)
            .children(self.tabs_error.as_ref().cloned().map(secondary))
            .when(self.tabs_error.is_none() && self.tabs.is_empty(), |div| {
                div.child(secondary(if self.controls_locked() {
                    "正在读取插件标签页…"
                } else {
                    "没有已注册的插件标签页"
                }))
            })
            .children(query)
            .when(can_cycle, |div| {
                div.child(secondary(
                    "焦点在文本框外时，可用 Tab / Shift+Tab 切换选项。",
                ))
            })
            .children(self.notice.as_ref().cloned().map(secondary))
            .children(self.query_error.as_ref().cloned().map(secondary))
            .children(edit)
            .children(sections)
    }
}

fn main() {
    if std::env::var_os("WAYLAND_DISPLAY").is_none() {
        eprintln!("DeskWaifu 控制面板需要 Wayland 会话（WAYLAND_DISPLAY）");
        std::process::exit(1);
    }
    let socket = std::env::var_os("DESKWAIFU_PANEL_SOCKET")
        .map(PathBuf::from)
        .unwrap_or_else(|| ipc::DEFAULT_SOCKET.into());
    gpui_kit::application().run(move |cx| {
        gpui_kit::init(cx);
        // 让原始按键监听器检查自动重复和文本焦点。
        cx.bind_keys([
            KeyBinding::new("tab", NoAction, Some("DeskWaifuCycle && !Input")),
            KeyBinding::new("shift-tab", NoAction, Some("DeskWaifuCycle && !Input")),
        ]);
        let options = WindowOptions {
            titlebar: Some(TitlebarOptions {
                title: Some("DeskWaifu 控制面板".into()),
                ..Default::default()
            }),
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                None,
                size(px(780.), px(760.)),
                cx,
            ))),
            ..Default::default()
        };
        if let Err(error) = gpui_kit::open_window(options, cx, |window, cx| {
            cx.new(|cx| {
                let mut panel = Panel {
                    socket,
                    focus: cx.focus_handle(),
                    tabs: Vec::new(),
                    selected: 0,
                    input: None,
                    dynamic: None,
                    page_bindings: Vec::new(),
                    binding_scan: None,
                    edit: None,
                    busy: false,
                    tabs_error: None,
                    query_error: None,
                    notice: None,
                };
                window.focus(&panel.focus, cx);
                panel.reconnect(window, cx);
                panel
            })
        }) {
            eprintln!("无法打开控制面板：{error:#}");
            std::process::exit(1);
        }
        cx.activate(true);
    });
}
