//! 与 UI、执行器和动态库 ABI 无关的进程内插件契约。
use std::{any::Any, any::TypeId, sync::Arc, time::Duration};

pub type PluginResult = Result<(), String>;
pub type CommandCallback = fn(&[String]) -> Result<String, String>;

#[derive(Clone)]
pub struct Command {
    pub name: String,
    pub help: Vec<String>,
    pub callback: CommandCallback,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PluginId(pub usize);

/// 消息负载共享所有权；运行时只负责路由，不解释领域内容。
#[derive(Clone)]
pub struct Message {
    pub sender: PluginId,
    payload: Arc<dyn Any + Send + Sync>,
}

impl Message {
    pub fn new<T: Any + Send + Sync>(sender: PluginId, payload: T) -> Self {
        Self {
            sender,
            payload: Arc::new(payload),
        }
    }

    pub fn payload<T: Any>(&self) -> Option<&T> {
        self.payload.downcast_ref()
    }

    pub fn message_type(&self) -> TypeId {
        self.payload.as_ref().type_id()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PublishError {
    Inactive,
    QueueFull,
}

impl std::fmt::Display for PublishError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{}",
            match self {
                Self::Inactive => "plugin context is inactive",
                Self::QueueFull => "message queue is full",
            }
        )
    }
}

impl std::error::Error for PublishError {}

/// 宿主适配接口；插件通常使用 Context 的类型化方法。
/// 这些 Rust 接口仅用于同一程序内静态组合，不是稳定动态库 ABI。
pub trait Host {
    fn id(&self) -> PluginId;
    fn subscribe(&mut self, message_type: TypeId) -> PluginResult;
    fn publish(&mut self, message: Message) -> Result<(), PublishError>;
    fn register_command(&mut self, command: Command) -> PluginResult;
}

pub struct Context<'a> {
    host: &'a mut dyn Host,
}

impl<'a> Context<'a> {
    pub fn new(host: &'a mut dyn Host) -> Self {
        Self { host }
    }
    pub fn id(&self) -> PluginId {
        self.host.id()
    }

    /// 仅在 initialize 阶段登记订阅；同一类型重复订阅不会重复投递。
    pub fn subscribe<T: Any + Send + Sync>(&mut self) -> PluginResult {
        self.host.subscribe(TypeId::of::<T>())
    }

    /// 返回成功仅表示入队；无订阅者也成功。不会同步调用其他插件。
    pub fn publish<T: Any + Send + Sync>(&mut self, payload: T) -> Result<(), PublishError> {
        self.host.publish(Message::new(self.id(), payload))
    }

    /// 仅在 initialize 阶段注册命令；名称必须是单个子命令标识。
    pub fn register_command(
        &mut self,
        name: impl Into<String>,
        help: Vec<String>,
        callback: crate::CommandCallback,
    ) -> PluginResult {
        self.host.register_command(crate::Command {
            name: name.into(),
            help,
            callback,
        })
    }
}

/// 所有回调由宿主在同一线程串行调用；不要在回调中阻塞。
/// stop 必须清理部分初始化状态，并等待插件拥有的工作线程退出。
pub trait Plugin {
    fn name(&self) -> &str;
    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult;
    fn start(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        Ok(())
    }
    /// None 表示没有周期任务。零周期无效；繁忙时跳过错过的 tick，不追赶。
    fn tick_interval(&self) -> Option<Duration> {
        None
    }
    fn tick(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        Ok(())
    }
    fn on_message(&mut self, message: &Message, cx: &mut Context<'_>) -> PluginResult;
    fn stop(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        Ok(())
    }
}
