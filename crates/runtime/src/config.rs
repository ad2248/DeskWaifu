//! 运行时共用的 JSONC 配置。顶层对象以插件 ID 为键，插件负责解释自己的值。

use std::collections::HashSet;
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};

use jsonc_parser::{ParseOptions, Scanner, ast, common::Ranged, tokens::Token};
use serde_json::{Map, Value};

const MAX_BYTES: usize = 1024 * 1024;
const MAX_DEPTH: usize = 64;
const PRIVATE_MODE: u32 = 0o600;
const EMPTY_DOCUMENT: &str = "{}\n";

/// 宿主进程内的配置快照。路径的上级目录应由当前用户控制。
///
/// 写入前重新读取磁盘文件，以保留此前完成的手动编辑。宿主应串行调用
/// `replace`，也应避免让外部编辑器与宿主同时写入。
pub struct ConfigStore {
    path: PathBuf,
    sections: Map<String, Value>,
}

impl ConfigStore {
    /// 加载 UTF-8 JSONC。文件缺失视为空配置，首次写入时才创建。
    ///
    /// 已有文件必须是权限恰为 `0600` 的普通文件。文档最多 1 MiB，
    /// 对象及数组总嵌套深度最多 64 层（计入顶层对象）。
    pub fn load(path: PathBuf) -> Result<Self, String> {
        let sections = match read_document(&path).map_err(|err| at_path(&path, err))? {
            Some(text) => parse_document(&text).map_err(|err| at_path(&path, err))?.1,
            None => Map::new(),
        };
        Ok(Self { path, sections })
    }

    /// 按插件 ID 读取快照中的配置。未配置的插件可自行使用默认值。
    pub fn section(&self, id: &str) -> Option<&Value> {
        self.sections.get(id)
    }

    /// 原子替换或新增整个区段，成功后刷新快照。
    ///
    /// 保留目标值之外的文本和注释。目标旧值内部的注释随该值一起替换。
    pub fn replace(&mut self, id: &str, value: Value) -> Result<(), String> {
        self.replace_inner(id, &value)
            .map_err(|err| at_path(&self.path, err))
    }

    fn replace_inner(&mut self, id: &str, value: &Value) -> Result<(), String> {
        check_value_depth(value, 1)?;
        let current = read_document(&self.path).map_err(|err| err.to_string())?;
        let text = current.as_deref().unwrap_or(EMPTY_DOCUMENT);
        let (object, _) = parse_document(text)?;
        let encoded = encode(value)?;
        let mut updated = text.to_owned();
        if let Some(property) = object.get(id) {
            let range = property.value.range();
            updated.replace_range(range.start..range.end, &encoded);
        } else {
            let close = object.range.end - 1;
            if id.len() > MAX_BYTES {
                return Err("config exceeds 1 MiB".into());
            }
            let key = encode(&Value::String(id.to_owned()))?;
            let (comma_at, trailing_comma) = if let Some(last) = object.properties.last() {
                let end = last.value.range().end;
                let mut scanner = Scanner::new(&text[end..close]);
                let mut trailing_comma = false;
                while let Some(token) = scanner.scan().map_err(|err| err.to_string())? {
                    trailing_comma |= matches!(token, Token::Comma);
                }
                ((!trailing_comma).then_some(end), trailing_comma)
            } else {
                (None, false)
            };
            let newline = if text.contains("\r\n") { "\r\n" } else { "\n" };
            let line_start = text[..close].rfind('\n').map_or(0, |index| index + 1);
            let on_own_line = text[line_start..close]
                .bytes()
                .all(|b| matches!(b, b' ' | b'\t'));
            let (insertion, prefix) = if on_own_line {
                (line_start, "")
            } else {
                (close, newline)
            };
            let comma = if trailing_comma { "," } else { "" };
            updated.insert_str(
                insertion,
                &format!("{prefix}  {key}: {encoded}{comma}{newline}"),
            );
            if let Some(end) = comma_at {
                updated.insert(end, ',');
            }
        }
        let (_, new_sections) = parse_document(&updated)?;
        persist(&self.path, updated.as_bytes(), current.is_some())
            .map_err(|err| err.to_string())?;
        self.sections = new_sections;
        Ok(())
    }
}

fn at_path(path: &Path, err: impl std::fmt::Display) -> String {
    format!("config {}: {err}", path.display())
}

fn read_document(path: &Path) -> io::Result<Option<String>> {
    // 先筛掉显然不是普通文件的节点，再用保护标志打开以防路径在检查后被替换。
    let before = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(err) => return Err(err),
    };
    check_file(&before)?;
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)?;
    let metadata = file.metadata()?;
    check_file(&metadata)?;
    if metadata.len() > MAX_BYTES as u64 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "config exceeds 1 MiB",
        ));
    }
    // 即使文件在读取期间增长，也只读上限加一个字节。
    let mut bytes = Vec::new();
    file.take(MAX_BYTES as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > MAX_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "config exceeds 1 MiB",
        ));
    }
    String::from_utf8(bytes)
        .map(Some)
        .map_err(|err| io::Error::new(io::ErrorKind::InvalidData, err))
}

fn check_file(metadata: &fs::Metadata) -> io::Result<()> {
    if !metadata.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "config must be a regular file",
        ));
    }
    if metadata.permissions().mode() & 0o7777 != PRIVATE_MODE {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "config permissions must be 0600",
        ));
    }
    Ok(())
}

fn parse_document(text: &str) -> Result<(ast::Object<'_>, Map<String, Value>), String> {
    if text.len() > MAX_BYTES {
        return Err("config exceeds 1 MiB".into());
    }
    // 0.26 的 AST 允许省略逗号。仅在用于 serde 校验的副本中移除注释和尾逗号。
    // 词法扫描也先约束递归解析的栈深度。注释和字符串中的括号不参与计数。
    let mut strict = text.as_bytes().to_vec();
    let mut scanner = Scanner::new(text);
    let mut depth = 0usize;
    let mut last_comma = None;
    while let Some(token) = scanner.scan().map_err(|err| err.to_string())? {
        let start = scanner.token_start();
        match token {
            Token::CommentLine(_) | Token::CommentBlock(_) => {
                for byte in &mut strict[start..scanner.token_end()] {
                    if !matches!(*byte, b'\r' | b'\n') {
                        *byte = b' ';
                    }
                }
                continue;
            }
            Token::Comma => {
                last_comma = Some(start);
                continue;
            }
            Token::OpenBrace | Token::OpenBracket => depth += 1,
            Token::CloseBrace | Token::CloseBracket => {
                depth = depth.saturating_sub(1);
                if let Some(comma) = last_comma {
                    strict[comma] = b' ';
                }
            }
            _ => {}
        }
        last_comma = None;
        if depth > MAX_DEPTH {
            return Err("config nesting exceeds 64 levels".into());
        }
    }
    let parsed = jsonc_parser::parse_to_ast(
        text,
        &Default::default(),
        &ParseOptions {
            allow_comments: true,
            allow_trailing_commas: true,
            allow_loose_object_property_names: false,
        },
    )
    .map_err(|err| err.to_string())?;
    let Some(ast::Value::Object(object)) = parsed.value else {
        return Err("config root must be an object".into());
    };
    let mut ids = HashSet::new();
    if object
        .properties
        .iter()
        .any(|property| !ids.insert(property.name.as_str()))
    {
        return Err("config contains a duplicate section ID".into());
    }
    let sections = serde_json::from_slice(&strict).map_err(|err| err.to_string())?;
    Ok((object, sections))
}

fn check_value_depth(value: &Value, depth: usize) -> Result<(), String> {
    let children: &mut dyn Iterator<Item = &Value> = match value {
        Value::Array(values) => &mut values.iter(),
        Value::Object(values) => &mut values.values(),
        _ => return Ok(()),
    };
    if depth >= MAX_DEPTH {
        return Err("config nesting exceeds 64 levels".into());
    }
    for child in children {
        check_value_depth(child, depth + 1)?;
    }
    Ok(())
}

struct BoundedBuffer(Vec<u8>);

impl Write for BoundedBuffer {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > MAX_BYTES - self.0.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "config exceeds 1 MiB",
            ));
        }
        self.0.extend_from_slice(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn encode(value: &Value) -> Result<String, String> {
    let mut buffer = BoundedBuffer(Vec::new());
    serde_json::to_writer(&mut buffer, value).map_err(|err| err.to_string())?;
    String::from_utf8(buffer.0).map_err(|err| err.to_string())
}

fn persist(path: &Path, bytes: &[u8], replace_existing: bool) -> io::Result<()> {
    let parent = path
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(parent)?;
    let mut temporary = tempfile::NamedTempFile::new_in(parent)?;
    // 即使 umask 额外限制了临时文件权限，也以准确的 0600 发布。
    temporary
        .as_file()
        .set_permissions(fs::Permissions::from_mode(PRIVATE_MODE))?;
    temporary.write_all(bytes)?;
    temporary.as_file().sync_all()?;
    if replace_existing {
        temporary.persist(path)?;
    } else {
        temporary.persist_noclobber(path)?;
    }
    Ok(())
}
