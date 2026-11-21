//! 插件契约。动态插件直接使用 Rust 接口，必须与宿主使用同一工具链及同版代码构建。
//! 不提供稳定 ABI；只加载可信插件。
use std::{any::Any, any::TypeId, future::Future, rc::Rc, sync::Arc, time::Duration};

mod os;
pub use os::{AudioStream, ClipboardWriteError, LocalFuture, OsError, OsServices};

/// 动态库导出的固定 Rust 函数名；不是 C ABI，也不承诺跨版本兼容。
pub const PLUGIN_ENTRY_SYMBOL: &[u8] = b"deskwaifu_plugin_create\0";
pub type PluginCreate = fn() -> Box<dyn Plugin>;

/// 为插件生成 Rust 动态加载入口，表达式应创建一个新的插件实例。
#[macro_export]
macro_rules! export_plugin {
    ($plugin:expr) => {
        // 唯一导出符号，仅供同工具链、同版本宿主使用。
        #[allow(unsafe_code)]
        #[unsafe(no_mangle)]
        pub fn deskwaifu_plugin_create() -> Box<dyn $crate::Plugin> {
            Box::new($plugin)
        }
    };
}

pub type PluginResult = Result<(), String>;

#[derive(Clone)]
pub struct Command {
    pub name: String,
    pub help: Vec<String>,
}

/// 由插件注册、由独立控制面板呈现的标签页描述。
pub struct PanelTab {
    pub id: String,
    pub title: String,
    pub order: i32,
    pub query: Option<PanelQuery>,
    pub sections: Vec<PanelSection>,
}

pub struct PanelQuery {
    /// 本插件的查询命令及其固定参数。
    pub args: Vec<String>,
    /// 若存在提示文字，面板可在查询命令末尾追加一个文本参数。
    pub input_placeholder: Option<String>,
}

pub struct PanelSection {
    pub title: String,
    pub actions: Vec<PanelAction>,
}

pub struct PanelAction {
    pub label: String,
    /// 完整命令参数，第一个元素是本插件注册的命令。
    pub args: Vec<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PluginId(pub usize);

/// 宿主为已接受的 IPC 连接分配的标识，在同一服务实例中不复用。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ConnectionId(pub u64);

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
/// 支持静态组合和同工具链构建的动态插件，不是稳定动态库 ABI。
pub trait Host {
    fn id(&self) -> PluginId;
    fn subscribe(&mut self, message_type: TypeId) -> PluginResult;
    fn publish(&mut self, message: Message) -> Result<(), PublishError>;
    fn register_command(&mut self, command: Command) -> PluginResult;
    fn register_panel_tab(&mut self, _tab: PanelTab) -> PluginResult {
        Err("panel tabs are not supported by this host".into())
    }
    fn config(&self) -> Option<serde_json::Value> {
        None
    }
    fn replace_config(&mut self, _value: serde_json::Value) -> PluginResult {
        Err("configuration storage is unavailable".into())
    }
    fn spawn_local(&mut self, _future: LocalFuture<'static, ()>) -> PluginResult {
        Err("local tasks are not available".into())
    }
    fn os(&self) -> Rc<dyn OsServices> {
        unavailable_os_services()
    }
}

#[doc(hidden)]
pub fn unavailable_os_services() -> Rc<dyn OsServices> {
    Rc::new(os::Unavailable)
}

pub struct Context<'a> {
    host: &'a mut dyn Host,
    connection_id: Option<ConnectionId>,
}

impl<'a> Context<'a> {
    pub fn new(host: &'a mut dyn Host) -> Self {
        Self::with_connection(host, None)
    }

    #[doc(hidden)]
    pub fn with_connection(host: &'a mut dyn Host, connection_id: Option<ConnectionId>) -> Self {
        Self {
            host,
            connection_id,
        }
    }

    /// 返回当前 IPC 命令的连接标识；直接派发及其他回调返回 None。
    pub fn connection_id(&self) -> Option<ConnectionId> {
        self.connection_id
    }

    pub fn id(&self) -> PluginId {
        self.host.id()
    }

    /// 返回可跨 await 持有的系统服务句柄。
    pub fn os(&self) -> Rc<dyn OsServices> {
        self.host.os()
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
    pub fn register_command(&mut self, name: impl Into<String>, help: Vec<String>) -> PluginResult {
        self.host.register_command(crate::Command {
            name: name.into(),
            help,
        })
    }

    /// 仅在 initialize 阶段注册。标签页 ID 在当前插件内唯一。
    pub fn register_panel_tab(&mut self, tab: PanelTab) -> PluginResult {
        self.host.register_panel_tab(tab)
    }

    /// 返回本插件自己的一级配置节点。缺省时由插件自行提供默认值。
    pub fn config(&self) -> Option<serde_json::Value> {
        self.host.config()
    }

    /// 持久化替换本插件配置节点。失败时保留之前保存的值。
    pub fn replace_config(&mut self, value: serde_json::Value) -> PluginResult {
        self.host.replace_config(value)
    }

    /// 提交由宿主在当前线程上驱动的任务。任务不会在此调用中立即执行。
    /// 只在运行阶段接受任务，并在 stop 回调前同步丢弃未完成的任务。
    pub fn spawn_local(&mut self, future: impl Future<Output = ()> + 'static) -> PluginResult {
        self.host.spawn_local(Box::pin(future))
    }
}

/// 所有回调由宿主在同一线程串行调用；不要在回调中阻塞。
/// stop 必须清理部分初始化状态，并等待插件拥有的工作线程退出。
pub trait Plugin {
    /// 不随运行或版本变化的英文标识，例如 `terminal` 或 `voice_input`。
    fn plugin_id(&self) -> &str;
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
    /// 处理本实例在 initialize 中注册的命令。args 不包含命令名。
    fn on_command(
        &mut self,
        name: &str,
        _args: &[String],
        _cx: &mut Context<'_>,
    ) -> Result<String, String> {
        Err(format!("unhandled command: {name}"))
    }
    /// 连接关闭后释放该连接持有的资源。未执行过命令的连接也可能收到通知。
    fn on_disconnect(&mut self, _connection_id: ConnectionId, _cx: &mut Context<'_>) {}
    fn on_message(&mut self, message: &Message, cx: &mut Context<'_>) -> PluginResult;
    fn stop(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        Ok(())
    }
}
