//! 单线程、有界 FIFO 的进程内插件运行时，不依赖 Qt 或异步执行器。
use deskwaifu_sdk::{
    Command, Context, Host, Message, Plugin, PluginId, PluginResult, PublishError,
};
use std::{
    any::TypeId,
    collections::{BTreeMap, HashSet, VecDeque},
    thread,
    time::{Duration, Instant},
};

struct Delivery {
    message: Message,
    targets: Vec<usize>,
}
struct Bus {
    active: bool,
    capacity: usize,
    subscriptions: Vec<HashSet<TypeId>>,
    queue: VecDeque<Delivery>,
}

struct ScopedHost<'a> {
    bus: &'a mut Bus,
    id: PluginId,
    initializing: bool,
    commands: &'a mut BTreeMap<String, Command>,
}
impl Host for ScopedHost<'_> {
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
        if command.name == "help" || self.commands.contains_key(&command.name) {
            return Err(format!("duplicate or reserved command: {}", command.name));
        }
        self.commands.insert(command.name.clone(), command);
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

struct Entry {
    plugin: Box<dyn Plugin>,
    initialized: bool,
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
    entries: Vec<Entry>,
    bus: Bus,
    state: State,
    elapsed: Duration,
    commands: BTreeMap<String, Command>,
}

impl Runtime {
    /// capacity 限制排队广播数，每个广播共享一份负载。
    pub fn new(capacity: usize) -> Self {
        Self {
            entries: Vec::new(),
            bus: Bus {
                active: false,
                capacity,
                subscriptions: Vec::new(),
                queue: VecDeque::new(),
            },
            state: State::Created,
            elapsed: Duration::ZERO,
            commands: BTreeMap::new(),
        }
    }

    pub fn add(&mut self, plugin: impl Plugin + 'static) -> Result<PluginId, String> {
        if self.state != State::Created {
            return Err("plugins must be added before start".into());
        }
        if self
            .entries
            .iter()
            .any(|entry| entry.plugin.name() == plugin.name())
        {
            return Err(format!("duplicate plugin: {}", plugin.name()));
        }
        let id = PluginId(self.entries.len());
        self.entries.push(Entry {
            plugin: Box::new(plugin),
            initialized: false,
            interval: None,
            next_tick: None,
        });
        self.bus.subscriptions.push(HashSet::new());
        Ok(id)
    }

    /// 所有 initialize 成功后才激活通信；所有 start 完成后才开始派发。
    /// 初始化/启动失败时关闭整个宿主，不留下部分运行的插件。
    pub fn start(&mut self) -> PluginResult {
        if self.state != State::Created {
            return Err("runtime cannot be started again".into());
        }
        let result = self.start_inner();
        self.finish_operation(result)
    }

    fn start_inner(&mut self) -> PluginResult {
        for (id, entry) in self.entries.iter_mut().enumerate() {
            entry.initialized = true;
            let mut host = ScopedHost {
                bus: &mut self.bus,
                id: PluginId(id),
                initializing: true,
                commands: &mut self.commands,
            };
            entry
                .plugin
                .initialize(&mut Context::new(&mut host))
                .map_err(|e| format!("{} initialize: {e}", entry.plugin.name()))?;
            let interval = entry.plugin.tick_interval();
            if interval == Some(Duration::ZERO) {
                return Err(format!("{}: zero tick interval", entry.plugin.name()));
            }
            entry.interval = interval;
            entry.next_tick = interval;
        }
        self.bus.active = true;
        self.state = State::Running;
        for (id, entry) in self.entries.iter_mut().enumerate() {
            let mut host = ScopedHost {
                bus: &mut self.bus,
                id: PluginId(id),
                initializing: false,
                commands: &mut self.commands,
            };
            entry
                .plugin
                .start(&mut Context::new(&mut host))
                .map_err(|e| format!("{} start: {e}", entry.plugin.name()))?;
        }
        Ok(())
    }

    /// 执行 CLI 来源的原始 argv（第一个参数为子命令）。
    pub fn dispatch_command(&self, argv: &[String]) -> Result<String, String> {
        if self.state != State::Running {
            return Err("runtime is not running".into());
        }
        let Some(name) = argv.first() else {
            return Err("missing command (try 'help')".into());
        };
        if name == "help" {
            return Ok(self.help_text());
        }
        let command = self
            .commands
            .get(name)
            .ok_or_else(|| format!("unknown command: {name}"))?;
        (command.callback)(&argv[1..])
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
        for (name, command) in &self.commands {
            for (index, text) in command.help.iter().enumerate() {
                lines.push(format!(
                    "{:<width$}  {}",
                    if index == 0 { name.as_str() } else { "" },
                    text
                ));
            }
            if command.help.is_empty() {
                lines.push(format!("{name:<width$}"));
            }
        }
        lines.join("\n")
    }

    /// 推进逻辑时钟并串行执行到期任务，供测试无需真实等待地驱动运行时。
    /// 每次最多派发 capacity 个广播，避免回调持续发消息导致永不返回。
    pub fn advance(&mut self, delta: Duration) -> PluginResult {
        if self.state != State::Running {
            return Err("runtime is not running".into());
        }
        let result = self.advance_inner(delta);
        self.finish_operation(result)
    }

    fn advance_inner(&mut self, delta: Duration) -> PluginResult {
        self.elapsed = self
            .elapsed
            .checked_add(delta)
            .ok_or("runtime clock overflow")?;
        for (id, entry) in self.entries.iter_mut().enumerate() {
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
                    .map_err(|e| format!("{} tick: {e}", entry.plugin.name()))?;
            }
        }
        for _ in 0..self.bus.capacity {
            let Some(delivery) = self.bus.queue.pop_front() else {
                break;
            };
            for id in delivery.targets {
                let entry = &mut self.entries[id];
                let mut host = ScopedHost {
                    bus: &mut self.bus,
                    id: PluginId(id),
                    initializing: false,
                    commands: &mut self.commands,
                };
                entry
                    .plugin
                    .on_message(&delivery.message, &mut Context::new(&mut host))
                    .map_err(|e| format!("{} message: {e}", entry.plugin.name()))?;
            }
        }
        Ok(())
    }

    /// 使用单调时钟实际运行指定时长；调用者随后显式 stop 以检查清理结果。
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

    /// 先统一失效、清空队列，再逆序停止所有尝试过初始化的插件。
    pub fn stop(&mut self) -> PluginResult {
        if self.state == State::Stopped {
            return Ok(());
        }
        self.state = State::Stopped;
        self.bus.active = false;
        self.bus.queue.clear();
        self.bus.subscriptions.iter_mut().for_each(HashSet::clear);
        let mut failures = Vec::new();
        for (id, entry) in self.entries.iter_mut().enumerate().rev() {
            if entry.initialized {
                let mut host = ScopedHost {
                    bus: &mut self.bus,
                    id: PluginId(id),
                    initializing: false,
                    commands: &mut self.commands,
                };
                if let Err(error) = entry.plugin.stop(&mut Context::new(&mut host)) {
                    failures.push(format!("{} stop: {error}", entry.plugin.name()));
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

#[cfg(test)]
mod tests;

impl Drop for Runtime {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}
