use super::{secondary, section_box};
use crate::ipc::{
    AcceptedOperation, CommandError, Connection, ConnectionGuard, OperationStatus, PanelEdit,
};
use gpui_kit::component::{
    Disableable,
    button::{Button, ButtonVariants},
    input::{Input, InputState},
};
use gpui_kit::*;
use std::path::Path;
use std::time::{Duration, Instant};

use super::Panel;

const HEARTBEAT: Duration = Duration::from_secs(3);
const POLL_INTERVAL: Duration = Duration::from_millis(200);
const COMPLETION_WAIT: Duration = Duration::from_secs(6);

pub struct EditSession {
    form: PanelEdit,
    fields: Vec<Entity<InputState>>,
    connection: Option<Connection>,
    completion: Option<Vec<String>>,
    heartbeat: Vec<String>,
    _guard: ConnectionGuard,
    _heartbeat_task: Task<()>,
}

#[derive(Clone, Copy)]
enum EditAction {
    Preview,
    Apply,
    Cancel,
}

impl EditAction {
    fn label(self) -> &'static str {
        match self {
            Self::Preview => "预览",
            Self::Apply => "应用",
            Self::Cancel => "取消",
        }
    }

    fn id(self) -> &'static str {
        match self {
            Self::Preview => "edit-preview",
            Self::Apply => "edit-apply",
            Self::Cancel => "edit-cancel",
        }
    }
}

impl Panel {
    fn begin_edit(&mut self, window: &Window, cx: &mut Context<Self>) {
        if self.controls_locked() {
            return;
        }
        let Some(page) = &self.dynamic else { return };
        let Some(form) = page.edit.clone() else {
            return;
        };
        let completion = page.completion_query.clone();
        let Some(heartbeat) = completion.clone().or_else(|| self.query_args(cx)) else {
            self.notice = Some("插件没有提供编辑会话的保活命令".into());
            cx.notify();
            return;
        };
        let socket = self.socket.clone();
        let begin = form.begin.clone();
        let completion_args = completion.clone();
        self.busy = true;
        self.notice = Some("正在开始编辑…".into());
        let pending =
            cx.background_spawn(
                async move { open_edit(&socket, &begin, completion_args.as_deref()) },
            );
        cx.spawn_in(window, async move |view, cx| {
            let result = pending.await;
            let _ = view.update_in(cx, |this, window, cx| {
                this.busy = false;
                match result {
                    Ok((connection, guard)) => {
                        let fields: Vec<_> = form
                            .fields
                            .iter()
                            .map(|field| {
                                cx.new(|cx| {
                                    InputState::new(window, cx).default_value(field.value.clone())
                                })
                            })
                            .collect();
                        if let Some(first) = fields.first() {
                            window.focus(&first.read(cx).focus_handle(cx), cx);
                        }
                        let heartbeat_task = cx.spawn(async move |view, cx| {
                            loop {
                                cx.background_executor().timer(HEARTBEAT).await;
                                let Ok(pending) = view.update(cx, |this, cx| this.heartbeat(cx))
                                else {
                                    break;
                                };
                                let Some(pending) = pending else { continue };
                                let (connection, response) = pending.await;
                                let Ok(keep) = view.update(cx, |this, cx| match response {
                                    Ok(_) => {
                                        if let Some(session) = &mut this.edit {
                                            session.connection = Some(connection);
                                        }
                                        true
                                    }
                                    Err(error) => {
                                        this.edit = None;
                                        this.notice = Some(format!("编辑连接已关闭：{error}"));
                                        this.query(cx);
                                        cx.notify();
                                        false
                                    }
                                }) else {
                                    break;
                                };
                                if !keep {
                                    break;
                                }
                            }
                        });
                        this.edit = Some(EditSession {
                            form,
                            fields,
                            connection: Some(connection),
                            completion,
                            heartbeat,
                            _guard: guard,
                            _heartbeat_task: heartbeat_task,
                        });
                        this.notice = Some("编辑中".into());
                    }
                    Err(error) => this.notice = Some(format!("无法开始编辑：{error}")),
                }
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn heartbeat(
        &mut self,
        cx: &mut Context<Self>,
    ) -> Option<Task<(Connection, Result<String, CommandError>)>> {
        let session = self.edit.as_mut()?;
        let mut connection = session.connection.take()?;
        let args = session.heartbeat.clone();
        Some(cx.background_spawn(async move {
            let response = connection.command(&args);
            (connection, response)
        }))
    }

    fn edit_action(&mut self, action: EditAction, window: &Window, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        let Some(session) = &mut self.edit else {
            return;
        };
        let Some(mut connection) = session.connection.take() else {
            self.notice = Some("正在检查编辑连接，请稍后重试".into());
            cx.notify();
            return;
        };
        let mut steps = Vec::with_capacity(2);
        if matches!(action, EditAction::Preview | EditAction::Apply) {
            let values = session
                .fields
                .iter()
                .map(|field| field.read(cx).value().to_string())
                .collect();
            steps.push(session.form.update_args(values));
        }
        match action {
            EditAction::Preview => {}
            EditAction::Apply => steps.push(session.form.apply.clone()),
            EditAction::Cancel => steps.push(session.form.cancel.clone()),
        }
        self.busy = true;
        self.notice = Some(format!("正在{}…", action.label()));
        let completion = session.completion.clone();
        let pending = cx.background_spawn(async move {
            let result = steps
                .iter()
                .try_for_each(|args| run_step(&mut connection, args, completion.as_deref()));
            (connection, result)
        });
        cx.spawn_in(window, async move |view, cx| {
            let (connection, result) = pending.await;
            let _ = view.update_in(cx, |this, window, cx| {
                this.busy = false;
                let finish = match result {
                    Ok(()) => {
                        this.notice = Some(format!("{}完成", action.label()));
                        !matches!(action, EditAction::Preview)
                    }
                    Err(CommandError::Rejected(error)) => {
                        this.notice = Some(format!("{}失败：{error}", action.label()));
                        false
                    }
                    Err(CommandError::Connection(error)) => {
                        this.notice = Some(format!("编辑连接已关闭：{error}"));
                        true
                    }
                };
                if finish {
                    this.edit = None;
                    window.focus(&this.focus, cx);
                    this.query(cx);
                } else if let Some(session) = &mut this.edit {
                    session.connection = Some(connection);
                }
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    pub(super) fn render_edit(&self, cx: &mut Context<Self>) -> Option<Div> {
        let session = match &self.edit {
            Some(session) => session,
            None => {
                self.dynamic.as_ref()?.edit.as_ref()?;
                return Some(
                    section_box("编辑").child(
                        Button::new("edit-begin")
                            .label("开始编辑")
                            .outline()
                            .disabled(self.busy)
                            .on_click(
                                cx.listener(|this, _, window, cx| this.begin_edit(window, cx)),
                            ),
                    ),
                );
            }
        };
        let fields = session
            .form
            .fields
            .iter()
            .zip(&session.fields)
            .map(|(field, input)| {
                div()
                    .min_w(px(150.))
                    .flex_1()
                    .child(secondary(field.label.clone()))
                    .child(
                        Input::new(input)
                            .id(SharedString::from(format!("edit-field-{}", field.id)))
                            .aria_label(field.label.clone()),
                    )
            });
        let buttons = [EditAction::Preview, EditAction::Apply, EditAction::Cancel]
            .into_iter()
            .map(|action| {
                let button = Button::new(action.id()).label(action.label());
                let button = if matches!(action, EditAction::Apply) {
                    button.primary()
                } else {
                    button.outline()
                };
                button.disabled(self.busy).on_click(
                    cx.listener(move |this, _, window, cx| this.edit_action(action, window, cx)),
                )
            });
        Some(
            section_box("编辑")
                .child(div().flex().flex_wrap().gap_2().children(fields))
                .child(div().flex().gap_2().children(buttons)),
        )
    }
}

fn open_edit(
    socket: &Path,
    begin: &[String],
    completion: Option<&[String]>,
) -> Result<(Connection, ConnectionGuard), CommandError> {
    let mut connection = Connection::connect(socket).map_err(CommandError::Connection)?;
    let guard = connection.guard().map_err(CommandError::Connection)?;
    run_step(&mut connection, begin, completion)?;
    Ok((connection, guard))
}

fn run_step(
    connection: &mut Connection,
    args: &[String],
    completion: Option<&[String]>,
) -> Result<(), CommandError> {
    let output = connection.command(args)?;
    let Ok(operation) = serde_json::from_str::<AcceptedOperation>(&output) else {
        return Ok(());
    };
    let id = operation.accepted_operation_id;
    let completion = completion
        .ok_or_else(|| CommandError::Connection(format!("插件未提供操作 #{id} 的结果查询")))?;
    let deadline = Instant::now() + COMPLETION_WAIT;
    while Instant::now() < deadline {
        let output = connection
            .command(completion)
            .map_err(|error| CommandError::Connection(error.to_string()))?;
        let status: OperationStatus = serde_json::from_str(&output)
            .map_err(|error| CommandError::Connection(error.to_string()))?;
        if let Some(operation) = status
            .snapshot
            .completed
            .into_iter()
            .find(|operation| operation.id == id)
        {
            return match operation.error {
                Some(error) => Err(CommandError::Rejected(error)),
                None => Ok(()),
            };
        }
        std::thread::sleep(POLL_INTERVAL);
    }
    Err(CommandError::Connection(format!(
        "未能确认操作 #{id} 的结果"
    )))
}
