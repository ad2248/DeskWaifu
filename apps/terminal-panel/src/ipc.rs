use serde::Deserialize;
use serde::de::DeserializeOwned;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::Shutdown;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::time::Duration;

pub const DEFAULT_SOCKET: &str = "/tmp/deskwaifu.socket";
const MAX_FRAME: usize = 64 * 1024;
const TIMEOUT: Duration = Duration::from_secs(2);

#[derive(Deserialize)]
pub struct PanelTabs {
    pub tabs: Vec<PanelTab>,
}

#[derive(Deserialize)]
pub struct PanelTab {
    pub plugin: String,
    pub id: String,
    pub title: String,
    #[serde(default)]
    pub sections: Vec<PanelSection>,
    pub query: Option<PanelQuery>,
}

#[derive(Deserialize)]
pub struct PanelQuery {
    pub args: Vec<String>,
    pub input_placeholder: Option<String>,
}

impl PanelQuery {
    pub fn request_args(&self, input: &str) -> Vec<String> {
        let mut args = self.args.clone();
        if self.input_placeholder.is_some() && !input.is_empty() {
            args.push(input.to_owned());
        }
        args
    }
}

#[derive(Deserialize)]
pub struct PanelView {
    pub sections: Vec<PanelSection>,
    pub completion_query: Option<Vec<String>>,
    pub edit: Option<PanelEdit>,
    #[serde(default, deserialize_with = "crate::bindings::deserialize")]
    pub bindings: Vec<crate::bindings::PanelBinding>,
}

#[derive(Clone, Deserialize)]
pub struct PanelEdit {
    pub begin: Vec<String>,
    pub update: Vec<String>,
    pub apply: Vec<String>,
    pub cancel: Vec<String>,
    pub fields: Vec<EditField>,
    pub optional_start: Option<usize>,
}

#[derive(Clone, Deserialize)]
pub struct EditField {
    pub id: String,
    pub label: String,
    pub value: String,
}

impl PanelEdit {
    pub fn update_args(&self, values: Vec<String>) -> Vec<String> {
        let mut args = self.update.clone();
        let mut values = values;
        if let Some(index) = self.optional_start
            && values
                .get(index..)
                .is_some_and(|tail| tail.iter().all(String::is_empty))
        {
            values.truncate(index);
        }
        args.extend(values);
        args
    }
}

#[derive(Deserialize)]
pub struct PanelSection {
    pub title: String,
    #[serde(default)]
    pub actions: Vec<PanelAction>,
    #[serde(default)]
    pub lines: Vec<String>,
    #[serde(default)]
    pub cycle: bool,
}

impl PanelSection {
    pub fn cycle_action(&self, reverse: bool) -> Option<&PanelAction> {
        if !self.cycle || self.actions.is_empty() {
            return None;
        }
        let count = self.actions.len();
        let selected = self.actions.iter().position(|action| action.selected);
        let index = match (selected, reverse) {
            (Some(index), false) => (index + 1) % count,
            (Some(index), true) => (index + count - 1) % count,
            (None, false) => 0,
            (None, true) => count - 1,
        };
        self.actions.get(index)
    }
}

#[derive(Deserialize)]
pub struct PanelAction {
    pub label: String,
    pub args: Vec<String>,
    #[serde(default)]
    pub selected: bool,
}

#[derive(Deserialize)]
pub struct AcceptedOperation {
    pub accepted_operation_id: u64,
}

#[derive(Deserialize)]
pub struct OperationStatus {
    pub snapshot: OperationSnapshot,
}

#[derive(Deserialize)]
pub struct OperationSnapshot {
    pub completed: Vec<Operation>,
}

#[derive(Deserialize)]
pub struct Operation {
    pub id: u64,
    pub error: Option<String>,
}

#[derive(Deserialize)]
#[serde(tag = "status", rename_all = "snake_case")]
enum Envelope {
    Ok { output: String },
    Error { error: String },
}

pub fn json_command<T: DeserializeOwned>(socket: &Path, payload: &[String]) -> Result<T, String> {
    let output = command(socket, payload)?;
    serde_json::from_str(&output).map_err(|error| format!("插件响应无效：{error}"))
}

pub fn command(socket: &Path, payload: &[String]) -> Result<String, String> {
    Connection::connect(socket)?
        .command(payload)
        .map_err(|error| error.to_string())
}

#[derive(Debug)]
pub enum CommandError {
    Rejected(String),
    Connection(String),
}

impl std::fmt::Display for CommandError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Rejected(message) | Self::Connection(message) => f.write_str(message),
        }
    }
}

pub struct Connection {
    socket: std::path::PathBuf,
    reader: BufReader<UnixStream>,
}

/// Interrupts an in-flight request when the editing window is closed.
pub struct ConnectionGuard(UnixStream);

impl Drop for ConnectionGuard {
    fn drop(&mut self) {
        let _ = self.0.shutdown(Shutdown::Both);
    }
}

impl Connection {
    pub fn connect(socket: &Path) -> Result<Self, String> {
        let io_error = |error| format!("IPC ({})：{error}", socket.display());
        let stream = UnixStream::connect(socket).map_err(io_error)?;
        stream.set_read_timeout(Some(TIMEOUT)).map_err(io_error)?;
        stream.set_write_timeout(Some(TIMEOUT)).map_err(io_error)?;
        Ok(Self {
            socket: socket.to_owned(),
            reader: BufReader::new(stream),
        })
    }

    pub fn guard(&self) -> Result<ConnectionGuard, String> {
        self.reader
            .get_ref()
            .try_clone()
            .map(ConnectionGuard)
            .map_err(|error| error.to_string())
    }

    pub fn command(&mut self, payload: &[String]) -> Result<String, CommandError> {
        let io_error =
            |error| CommandError::Connection(format!("IPC ({})：{error}", self.socket.display()));
        let mut frame =
            serde_json::to_vec(&serde_json::json!({"source": "cli", "payload": payload}))
                .map_err(|error| CommandError::Rejected(error.to_string()))?;
        if frame.len() > MAX_FRAME {
            return Err(CommandError::Rejected("IPC 请求超过 64 KiB".into()));
        }
        frame.push(b'\n');
        self.reader.get_mut().write_all(&frame).map_err(io_error)?;
        frame.clear();
        self.reader
            .by_ref()
            .take((MAX_FRAME + 1) as u64)
            .read_until(b'\n', &mut frame)
            .map_err(io_error)?;
        if frame.pop() != Some(b'\n') {
            return Err(CommandError::Connection("IPC 响应超长或缺少换行符".into()));
        }
        match serde_json::from_slice(&frame)
            .map_err(|error| CommandError::Connection(format!("IPC 响应无效：{error}")))?
        {
            Envelope::Ok { output } => Ok(output),
            Envelope::Error { error } => Err(CommandError::Rejected(error)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn query_keeps_input_as_one_argument() {
        // 用包含空格和换行的中文值验证输入作为一个原样参数附加，空值省略。
        let query = PanelQuery {
            args: vec!["plugin".into(), "query".into()],
            input_placeholder: Some("目录".into()),
        };
        assert_eq!(
            query.request_args("测试 目录\n模型"),
            ["plugin", "query", "测试 目录\n模型"]
        );
        assert_eq!(query.request_args(""), ["plugin", "query"]);
        let without_input = PanelQuery {
            input_placeholder: None,
            ..query
        };
        assert_eq!(without_input.request_args("ignored"), ["plugin", "query"]);
    }

    #[test]
    fn edit_omits_only_a_completely_empty_optional_group() {
        // 模拟三个必填、四个可选字段，确认全空后缀才省略且部分输入的位置不移位。
        let form: PanelEdit = serde_json::from_value(json!({
            "begin": ["fixture", "begin"], "update": ["fixture", "update"],
            "apply": ["fixture", "apply"], "cancel": ["fixture", "cancel"],
            "fields": [], "optional_start": 3,
        }))
        .unwrap();
        let args = |values: [&str; 7]| form.update_args(values.map(str::to_owned).to_vec());
        assert_eq!(
            args(["1", "2", "3", "", "", "", ""]),
            ["fixture", "update", "1", "2", "3"]
        );
        assert_eq!(
            args(["1", "2", "3", "", "5", "", ""]),
            ["fixture", "update", "1", "2", "3", "", "5", "", ""]
        );
    }

    #[test]
    fn guard_disconnects_an_existing_edit_connection() {
        // 用同一对 UnixStream 收到连续命令和回复，再关闭窗口守卫并验证服务端读到 EOF。
        let (client, server) = UnixStream::pair().unwrap();
        client.set_read_timeout(Some(TIMEOUT)).unwrap();
        server.set_read_timeout(Some(TIMEOUT)).unwrap();
        let mut connection = Connection {
            socket: "test".into(),
            reader: BufReader::new(client),
        };
        let guard = connection.guard().unwrap();
        let peer = std::thread::spawn(move || {
            let mut reader = BufReader::new(server);
            let mut line = String::new();
            for command in ["begin", "status", "update"] {
                line.clear();
                reader.read_line(&mut line).unwrap();
                assert_eq!(
                    serde_json::from_str::<serde_json::Value>(&line).unwrap()["payload"],
                    json!(["fixture", command])
                );
                reader
                    .get_mut()
                    .write_all(b"{\"status\":\"ok\",\"output\":\"done\"}\n")
                    .unwrap();
            }
            line.clear();
            assert_eq!(reader.read_line(&mut line).unwrap(), 0);
        });
        for command in ["begin", "status", "update"] {
            assert_eq!(
                connection
                    .command(&["fixture".into(), command.into()])
                    .unwrap(),
                "done"
            );
        }
        drop(guard);
        peer.join().unwrap();
    }

    #[test]
    fn cycle_selects_neighbor_and_wraps() {
        // 模拟插件声明的三个操作，检查选中项前后切换、边界回绕和默认字段。
        let mut section: PanelSection = serde_json::from_value(json!({
            "title": "模型",
            "cycle": true,
            "actions": [
                {"label": "甲", "args": ["fixture", "甲"], "selected": true},
                {"label": "乙", "args": ["fixture", "乙"]},
                {"label": "丙", "args": ["fixture", "丙"]},
            ],
        }))
        .unwrap();
        assert_eq!(section.cycle_action(false).unwrap().args[1], "乙");
        assert_eq!(section.cycle_action(true).unwrap().args[1], "丙");
        section.actions[0].selected = false;
        assert_eq!(section.cycle_action(false).unwrap().args[1], "甲");
        assert_eq!(section.cycle_action(true).unwrap().args[1], "丙");
        section.cycle = false;
        assert!(section.cycle_action(false).is_none());
    }
}
