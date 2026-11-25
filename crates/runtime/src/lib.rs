//! 单线程、有界 FIFO 的进程内插件运行时，不依赖 Qt 或异步执行器。
use deskwaifu_sdk::{
    Command, ConnectionId, Context, Host, LocalFuture, Message, OsServices, PanelTab, Plugin,
    PluginId, PluginResult, PublishError,
};
mod config;
use config::ConfigStore;

use futures_util::{Stream, stream::FuturesUnordered};
use std::{
    any::TypeId,
    collections::{BTreeMap, HashSet, VecDeque},
    pin::Pin,
    rc::Rc,
    task::{self, Poll},
    thread,
    time::{Duration, Instant},
};

// 将动态库操作的 unsafe 边界限制在一个模块内。
#[allow(unsafe_code)]
mod dynamic;

struct Delivery {
    message: Message,
    targets: Vec<usize>,
}
struct Bus {
    os: Option<Rc<dyn OsServices>>,
    active: bool,
    capacity: usize,
    subscriptions: Vec<HashSet<TypeId>>,
    queue: VecDeque<Delivery>,
    tasks: FuturesUnordered<LocalFuture<'static, ()>>,
    panel_tabs: Vec<(PluginId, PanelTab)>,
    plugin_ids: Vec<String>,
    config: Option<ConfigStore>,
}

type Commands = BTreeMap<String, (PluginId, Vec<String>)>;

struct ScopedHost<'a> {
    bus: &'a mut Bus,
    id: PluginId,
    initializing: bool,
    commands: &'a mut Commands,
}
impl Host for ScopedHost<'_> {
    fn os(&self) -> Rc<dyn OsServices> {
        self.bus
            .os
            .clone()
            .unwrap_or_else(deskwaifu_sdk::unavailable_os_services)
    }

    fn id(&self) -> PluginId {
        self.id
    }
    fn subscribe(&mut self, message_type: TypeId) -> PluginResult {
        if !self.initializing {
            return Err("subscriptions require initialization".into());
        }
        self.bus.subscriptions[self.id.0].insert(message_type);
        Ok(())
    }
    fn register_command(&mut self, command: Command) -> PluginResult {
        if !self.initializing {
            return Err("commands require initialization".into());
        }
        if command.name.is_empty() || command.name.chars().any(char::is_whitespace) {
            return Err("command name must be a non-empty token".into());
        }
        if matches!(command.name.as_str(), "help" | "panel-tabs")
            || self.commands.contains_key(&command.name)
        {
            return Err(format!("duplicate or reserved command: {}", command.name));
        }
        self.commands.insert(command.name, (self.id, command.help));
        Ok(())
    }
    fn register_panel_tab(&mut self, tab: PanelTab) -> PluginResult {
        if !self.initializing {
            return Err("panel tabs require initialization".into());
        }
        if tab.id.is_empty()
            || tab.id.len() > 64
            || !tab
                .id
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
            || !panel_text(&tab.title)
            || tab.query.as_ref().is_some_and(|query| {
                !panel_args(&query.args)
                    || query
                        .input_placeholder
                        .as_deref()
                        .is_some_and(|text| !panel_text(text))
            })
            || tab.sections.len() > 8
            || tab.sections.iter().any(|section| {
                !panel_text(&section.title)
                    || section.actions.len() > 8
                    || section
                        .actions
                        .iter()
                        .any(|action| !panel_text(&action.label) || !panel_args(&action.args))
            })
        {
            return Err("invalid panel tab description".into());
        }
        if self.bus.panel_tabs.len() >= 64
            || self
                .bus
                .panel_tabs
                .iter()
                .any(|(owner, existing)| *owner == self.id && existing.id == tab.id)
        {
            return Err("duplicate panel tab or registration limit reached".into());
        }
        self.bus.panel_tabs.push((self.id, tab));
        Ok(())
    }
    fn config(&self) -> Option<serde_json::Value> {
        self.bus
            .config
            .as_ref()?
            .section(&self.bus.plugin_ids[self.id.0])
            .cloned()
    }

    fn replace_config(&mut self, value: serde_json::Value) -> PluginResult {
        if !self.bus.active {
            return Err("configuration writes require an active plugin context".into());
        }
        self.bus
            .config
            .as_mut()
            .ok_or("configuration storage is unavailable")?
            .replace(&self.bus.plugin_ids[self.id.0], value)
    }
    fn spawn_local(&mut self, future: LocalFuture<'static, ()>) -> PluginResult {
        if !self.bus.active {
            return Err("local tasks require an active plugin context".into());
        }
        self.bus.tasks.push(future);
        Ok(())
    }
    fn publish(&mut self, message: Message) -> Result<(), PublishError> {
        if !self.bus.active {
            return Err(PublishError::Inactive);
        }
        let targets: Vec<_> = self
            .bus
            .subscriptions
            .iter()
            .enumerate()
            .filter(|(_, types)| types.contains(&message.message_type()))
            .map(|(id, _)| id)
            .collect();
        if targets.is_empty() {
            return Ok(());
        }
        if self.bus.queue.len() >= self.bus.capacity {
            return Err(PublishError::QueueFull);
        }
        self.bus.queue.push_back(Delivery { message, targets });
        Ok(())
    }
}

fn panel_args(args: &[String]) -> bool {
    !args.is_empty()
        && args.len() <= 8
        && args
            .iter()
            .all(|arg| arg.len() <= 256 && !arg.contains('\0'))
}

fn panel_text(value: &str) -> bool {
    !value.trim().is_empty() && value.len() <= 128 && !value.chars().any(char::is_control)
}

struct Entry {
    plugin: Box<dyn Plugin>,
    initialization_attempted: bool,
    interval: Option<Duration>,
    next_tick: Option<Duration>,
}
#[derive(PartialEq)]
enum State {
    Created,
    Running,
    Stopped,
}

pub struct Runtime {
    // 保留空槽，以免跳过插件后改变其他插件的 ID。
    entries: Vec<Option<Entry>>,
    bus: Bus,
    state: State,
    elapsed: Duration,
    commands: Commands,
    // 必须最后释放：实例、队列负载和任务的析构代码都可能位于动态库。
    libraries: Vec<libloading::Library>,
}

impl Runtime {
    /// capacity 限制排队广播数，每个广播共享一份负载。
    pub fn new(capacity: usize) -> Self {
        Self {
            entries: Vec::new(),
            bus: Bus {
                os: None,
                active: false,
                capacity,
                subscriptions: Vec::new(),
                queue: VecDeque::new(),
                tasks: FuturesUnordered::new(),
                panel_tabs: Vec::new(),
                plugin_ids: Vec::new(),
                config: None,
            },
            state: State::Created,
            elapsed: Duration::ZERO,
            commands: BTreeMap::new(),
            libraries: Vec::new(),
        }
    }

    /// 在插件初始化之前配置全局系统服务。
    pub fn set_os_services(&mut self, os: Rc<dyn OsServices>) -> PluginResult {
        if self.state != State::Created {
            return Err("system services must be set before start".into());
        }
        self.bus.os = Some(os);
        Ok(())
    }

    /// 从指定 JSONC 加载所有插件配置，须在 start 前调用。
    pub fn load_config(&mut self, path: std::path::PathBuf) -> PluginResult {
        if self.state != State::Created {
            return Err("configuration must be loaded before start".into());
        }
        self.bus.config = Some(ConfigStore::load(path)?);
        Ok(())
    }

    pub fn add(&mut self, plugin: impl Plugin + 'static) -> Result<PluginId, String> {
        self.add_boxed(Box::new(plugin))
    }

    fn add_boxed(&mut self, plugin: Box<dyn Plugin>) -> Result<PluginId, String> {
        if self.state != State::Created {
            return Err("plugins must be added before start".into());
        }
        let key = plugin.plugin_id();
        let mut bytes = key.bytes();
        if key.len() > 64
            || !bytes.next().is_some_and(|byte| byte.is_ascii_lowercase())
            || !bytes.all(|byte| {
                byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'_' | b'-')
            })
            || (key.len() == 36
                && key.bytes().enumerate().all(|(index, byte)| {
                    if matches!(index, 8 | 13 | 18 | 23) {
                        byte == b'-'
                    } else {
                        byte.is_ascii_hexdigit()
                    }
                }))
        {
            return Err("plugin ID must be a readable lowercase English identifier".into());
        }
        if self.bus.plugin_ids.iter().any(|id| id == key) {
            return Err(format!("duplicate plugin: {key}"));
        }
        self.bus.plugin_ids.push(key.to_owned());
        let id = PluginId(self.entries.len());
        self.entries.push(Some(Entry {
            plugin,
            initialization_attempted: false,
            interval: None,
            next_tick: None,
        }));
        self.bus.subscriptions.push(HashSet::new());
        Ok(id)
    }

    /// 跳过初始化失败的插件，再统一激活通信并启动其余插件。
    /// 启动回调返回错误时关闭整个宿主。
    pub fn start(&mut self) -> PluginResult {
        if self.state != State::Created {
            return Err("runtime cannot be started again".into());
        }
        let result = self.start_inner();
        self.finish_operation(result)
    }

    fn start_inner(&mut self) -> PluginResult {
        for (id, slot) in self.entries.iter_mut().enumerate() {
            let Some(entry) = slot else { continue };
            entry.initialization_attempted = true;
            let mut host = ScopedHost {
                bus: &mut self.bus,
                id: PluginId(id),
                initializing: true,
                commands: &mut self.commands,
            };
            let initialized = entry
                .plugin
                .initialize(&mut Context::new(&mut host))
                .and_then(|()| {
                    let interval = entry.plugin.tick_interval();
                    if interval == Some(Duration::ZERO) {
                        return Err("zero tick interval".into());
                    }
                    entry.interval = interval;
                    entry.next_tick = interval;
                    Ok(())
                });
            if let Err(error) = initialized {
                let name = entry.plugin.plugin_id().to_owned();
                eprintln!("{name} initialize: {error}; skipping plugin");
                host.initializing = false;
                host.bus.subscriptions[id].clear();
                host.commands.retain(|_, (owner, _)| *owner != host.id);
                host.bus.panel_tabs.retain(|(owner, _)| *owner != host.id);
                if let Err(error) = entry.plugin.stop(&mut Context::new(&mut host)) {
                    eprintln!("{name} stop after failed initialization: {error}");
                }
                *slot = None;
            }
        }
        self.bus.active = true;
        self.state = State::Running;
        for (id, entry) in self.entries.iter_mut().enumerate() {
            let Some(entry) = entry else { continue };
            let mut host = ScopedHost {
                bus: &mut self.bus,
                id: PluginId(id),
                initializing: false,
                commands: &mut self.commands,
            };
            entry
                .plugin
                .start(&mut Context::new(&mut host))
                .map_err(|e| format!("{} start: {e}", entry.plugin.plugin_id()))?;
        }
        Ok(())
    }

    /// 执行没有 IPC 连接的原始 argv（第一个参数为子命令）。
    pub fn dispatch_command(&mut self, argv: &[String]) -> Result<String, String> {
        self.dispatch_command_inner(None, argv)
    }

    /// 派发来自指定 IPC 连接的命令。
    pub fn dispatch_connection_command(
        &mut self,
        connection_id: ConnectionId,
        argv: &[String],
    ) -> Result<String, String> {
        self.dispatch_command_inner(Some(connection_id), argv)
    }

    fn dispatch_command_inner(
        &mut self,
        connection_id: Option<ConnectionId>,
        argv: &[String],
    ) -> Result<String, String> {
        if self.state != State::Running {
            return Err("runtime is not running".into());
        }
        let Some(name) = argv.first() else {
            return Err("missing command (try 'help')".into());
        };
        if name == "help" {
            return Ok(self.help_text());
        }
        if name == "panel-tabs" {
            if argv.len() != 1 {
                return Err("panel-tabs does not accept arguments".into());
            }
            return self.panel_tabs_json();
        }
        let id = self
            .commands
            .get(name)
            .map(|(owner, _)| *owner)
            .ok_or_else(|| format!("unknown command: {name}"))?;
        let entry = self.entries[id.0].as_mut().expect("registered plugin");
        let mut host = ScopedHost {
            bus: &mut self.bus,
            id,
            initializing: false,
            commands: &mut self.commands,
        };
        entry.plugin.on_command(
            name,
            &argv[1..],
            &mut Context::with_connection(&mut host, connection_id),
        )
    }

    /// 通知所有运行中的插件释放指定连接拥有的状态。
    pub fn disconnect_connection(&mut self, connection_id: ConnectionId) {
        if self.state != State::Running {
            return;
        }
        for (id, entry) in self.entries.iter_mut().enumerate() {
            let Some(entry) = entry else { continue };
            let mut host = ScopedHost {
                bus: &mut self.bus,
                id: PluginId(id),
                initializing: false,
                commands: &mut self.commands,
            };
            entry
                .plugin
                .on_disconnect(connection_id, &mut Context::new(&mut host));
        }
    }

    fn panel_tabs_json(&self) -> Result<String, String> {
        use serde_json::json;
        let mut tabs = self
            .bus
            .panel_tabs
            .iter()
            .filter_map(|(owner, tab)| {
                self.entries[owner.0].as_ref()?;
                Some((self.bus.plugin_ids[owner.0].as_str(), owner, tab))
            })
            .collect::<Vec<_>>();
        tabs.sort_by_key(|(plugin, _, tab)| (tab.order, *plugin, tab.id.as_str()));
        let tabs = tabs
            .into_iter()
            .map(|(plugin, owner, tab)| {
                let sections = tab
                    .sections
                    .iter()
                    .map(|section| {
                        let actions = section
                            .actions
                            .iter()
                            .filter(|action| {
                                self.commands
                                    .get(&action.args[0])
                                    .is_some_and(|(registered, _)| registered == owner)
                            })
                            .map(|action| json!({"label": action.label, "args": action.args}))
                            .collect::<Vec<_>>();
                        json!({"title": section.title, "actions": actions})
                    })
                    .collect::<Vec<_>>();
                let query = tab.query.as_ref().filter(|query| {
                    self.commands.get(&query.args[0]).is_some_and(|(registered, _)| registered == owner)
                }).map(|query| json!({"args": query.args, "input_placeholder": query.input_placeholder}));
                json!({"plugin": plugin, "id": tab.id, "title": tab.title, "sections": sections, "query": query})
            })
            .collect::<Vec<_>>();
        let payload = json!({"tabs": tabs}).to_string();
        let escaped = payload
            .bytes()
            .filter(|byte| matches!(byte, b'"' | b'\\'))
            .count();
        if payload.len() + escaped + r#"{"status":"ok","output":""}"#.len() > 64 * 1024 {
            return Err("panel tabs exceed the IPC frame limit".into());
        }
        Ok(payload)
    }

    /// 在宿主事件循环上驱动已提交任务，单次报告一个任务完成。
    /// 无任务可完成时挂起；调用者需在派发回调后重新轮询，以处理新任务。
    pub fn poll_local_task(&mut self, cx: &mut task::Context<'_>) -> Poll<()> {
        match Pin::new(&mut self.bus.tasks).poll_next(cx) {
            Poll::Ready(Some(())) => Poll::Ready(()),
            Poll::Pending | Poll::Ready(None) => Poll::Pending,
        }
    }

    /// 输出内建 help 与已注册子命令，按最长命令名对齐。
    pub fn help_text(&self) -> String {
        let width = self
            .commands
            .keys()
            .map(String::len)
            .max()
            .unwrap_or(4)
            .max(4);
        let mut lines = vec![format!("{:<width$}  {}", "help", "Show available commands")];
        for (name, (_, help)) in &self.commands {
            for (index, text) in help.iter().enumerate() {
                lines.push(format!(
                    "{:<width$}  {}",
                    if index == 0 { name.as_str() } else { "" },
                    text
                ));
            }
            if help.is_empty() {
                lines.push(format!("{name:<width$}"));
            }
        }
        lines.join("\n")
    }

    /// 推进逻辑时钟并串行执行到期任务，供测试无需真实等待地驱动运行时。
    /// 每次最多派发 capacity 个广播，避免回调持续发消息导致永不返回。
    pub fn advance(&mut self, delta: Duration) -> PluginResult {
        self.advance_with_shutdown(delta, |_| {})
    }

    /// 推进时钟；若运行中的插件发生错误，在 stop 前允许宿主清理外部资源。
    pub fn advance_with_shutdown(
        &mut self,
        delta: Duration,
        before_stop: impl FnOnce(&mut Self),
    ) -> PluginResult {
        if self.state != State::Running {
            return Err("runtime is not running".into());
        }
        let result = self.advance_inner(delta);
        if result.is_err() {
            before_stop(self);
        }
        self.finish_operation(result)
    }

    fn advance_inner(&mut self, delta: Duration) -> PluginResult {
        self.elapsed = self
            .elapsed
            .checked_add(delta)
            .ok_or("runtime clock overflow")?;
        for (id, entry) in self.entries.iter_mut().enumerate() {
            let Some(entry) = entry else { continue };
            if entry.next_tick.is_some_and(|due| due <= self.elapsed) {
                entry.next_tick = Some(
                    self.elapsed
                        .checked_add(entry.interval.unwrap())
                        .ok_or("timer overflow")?,
                );
                let mut host = ScopedHost {
                    bus: &mut self.bus,
                    id: PluginId(id),
                    initializing: false,
                    commands: &mut self.commands,
                };
                entry
                    .plugin
                    .tick(&mut Context::new(&mut host))
                    .map_err(|e| format!("{} tick: {e}", entry.plugin.plugin_id()))?;
            }
        }
        for _ in 0..self.bus.capacity {
            let Some(delivery) = self.bus.queue.pop_front() else {
                break;
            };
            for id in delivery.targets {
                let entry = self.entries[id].as_mut().expect("subscribed plugin");
                let mut host = ScopedHost {
                    bus: &mut self.bus,
                    id: PluginId(id),
                    initializing: false,
                    commands: &mut self.commands,
                };
                entry
                    .plugin
                    .on_message(&delivery.message, &mut Context::new(&mut host))
                    .map_err(|e| format!("{} message: {e}", entry.plugin.plugin_id()))?;
            }
        }
        Ok(())
    }

    /// 返回距离下次调度所需的逻辑时间，供外部事件循环按需等待。
    /// 有待派发消息时立即调度；没有消息或周期任务时无需定时唤醒。
    /// 调用者应从最近一次 advance 的采样时刻计算截止时间，扣除回调耗时。
    pub fn next_wakeup(&self) -> Option<Duration> {
        if self.state != State::Running {
            return None;
        }
        if !self.bus.queue.is_empty() {
            return Some(Duration::ZERO);
        }
        self.entries
            .iter()
            .flatten()
            .filter_map(|entry| entry.next_tick)
            .min()
            .map(|due| due.saturating_sub(self.elapsed))
    }

    /// 使用单调时钟同步运行周期回调和消息派发；本地异步任务由宿主事件循环驱动。
    /// 调用者随后显式 stop 以检查清理结果。
    pub fn run_for(&mut self, duration: Duration) -> PluginResult {
        if self.state != State::Running {
            return Err("runtime is not running".into());
        }
        let start = Instant::now();
        let mut previous = start;
        loop {
            let now = Instant::now();
            self.advance(now.duration_since(previous))?;
            previous = now;
            let elapsed = start.elapsed();
            if elapsed >= duration {
                return Ok(());
            }
            if !self.bus.queue.is_empty() {
                continue;
            }
            thread::sleep(self.wait_duration(previous.elapsed(), duration - elapsed));
        }
    }

    // elapsed 在本轮回调执行前采样；等待前必须扣除回调已消耗的真实时间。
    fn wait_duration(&self, callback_time: Duration, remaining: Duration) -> Duration {
        self.entries
            .iter()
            .flatten()
            .filter_map(|entry| entry.next_tick)
            .map(|due| {
                due.saturating_sub(self.elapsed)
                    .saturating_sub(callback_time)
            })
            .min()
            .unwrap_or(remaining)
            .min(remaining)
    }

    fn finish_operation(&mut self, result: PluginResult) -> PluginResult {
        match result {
            Ok(()) => Ok(()),
            Err(error) => match self.stop() {
                Ok(()) => Err(error),
                Err(cleanup) => Err(format!("{error}; shutdown: {cleanup}")),
            },
        }
    }

    /// 先统一失效、丢弃本地任务并清空队列，再逆序停止剩余已初始化插件。
    pub fn stop(&mut self) -> PluginResult {
        if self.state == State::Stopped {
            return Ok(());
        }
        self.state = State::Stopped;
        self.bus.active = false;
        self.bus.tasks.clear();
        self.bus.queue.clear();
        self.bus.panel_tabs.clear();
        self.bus.subscriptions.iter_mut().for_each(HashSet::clear);
        let mut failures = Vec::new();
        for (id, entry) in self.entries.iter_mut().enumerate().rev() {
            if let Some(entry) = entry
                .as_mut()
                .filter(|entry| entry.initialization_attempted)
            {
                let mut host = ScopedHost {
                    bus: &mut self.bus,
                    id: PluginId(id),
                    initializing: false,
                    commands: &mut self.commands,
                };
                if let Err(error) = entry.plugin.stop(&mut Context::new(&mut host)) {
                    failures.push(format!("{} stop: {error}", entry.plugin.plugin_id()));
                }
            }
        }
        if failures.is_empty() {
            Ok(())
        } else {
            Err(failures.join("; "))
        }
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}
