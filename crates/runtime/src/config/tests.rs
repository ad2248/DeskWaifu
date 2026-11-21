use super::*;
use serde_json::json;
use std::os::unix::{fs::symlink, net::UnixListener};

fn write_private(path: &Path, bytes: impl AsRef<[u8]>) {
    fs::write(path, bytes).unwrap();
    fs::set_permissions(path, fs::Permissions::from_mode(PRIVATE_MODE)).unwrap();
}

#[test]
fn missing_config_creates_private_file_on_first_write() {
    // 缺失文件走默认值，首次写入才创建父目录和权限为 0600 的文件。
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("new").join("config.jsonc");
    let mut store = ConfigStore::load(path.clone()).unwrap();
    assert_eq!(store.section("terminal"), None);
    assert!(!path.parent().unwrap().exists());

    let value = json!({ "enabled": true });
    store.replace("terminal", value.clone()).unwrap();
    assert_eq!(store.section("terminal"), Some(&value));
    assert_eq!(
        fs::metadata(&path).unwrap().permissions().mode() & 0o7777,
        0o600
    );
    assert_eq!(
        ConfigStore::load(path).unwrap().section("terminal"),
        Some(&value)
    );
}

#[test]
fn replacement_preserves_every_byte_outside_the_old_value() {
    // 带转义的键也命中同一个 ID，区段外围和其它插件的文本逐字节保留。
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("config.jsonc");
    let prefix = "// 共享配置\r\n{\r\n  // 终端设置\r\n  \"termi\\u006eal\": /* 值之前 */ ";
    let old_value = "{\r\n    // 旧值内部\r\n    \"enabled\": false,\r\n  }";
    let suffix =
        ", // 值之后\r\n  \"desktop\": { /* 桌面设置 */ \"scale\": 2, },\r\n}\r\n// 文件尾\r\n";
    write_private(&path, format!("{prefix}{old_value}{suffix}"));

    let mut store = ConfigStore::load(path.clone()).unwrap();
    assert_eq!(
        store.section("terminal"),
        Some(&json!({ "enabled": false }))
    );
    store
        .replace("terminal", json!({ "enabled": true }))
        .unwrap();
    assert_eq!(
        fs::read_to_string(path).unwrap(),
        format!("{prefix}{{\"enabled\":true}}{suffix}")
    );
    assert_eq!(store.section("desktop"), Some(&json!({ "scale": 2 })));
}

#[test]
fn inserts_new_section_with_or_without_trailing_comma() {
    // 插入时保留前一区段行末的注释，并兼容空对象、紧凑对象、尾逗号和 CRLF。
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("config.jsonc");
    for (original, expected) in [
        (
            "{\n  // 空配置\n}\n",
            "{\n  // 空配置\n  \"new\": null\n}\n",
        ),
        ("{\"old\":1}", "{\"old\":1,\n  \"new\": null\n}"),
        (
            "{\r\n  \"old\": 1 // 原注释\r\n}\r\n",
            "{\r\n  \"old\": 1, // 原注释\r\n  \"new\": null\r\n}\r\n",
        ),
        (
            "{\r\n  \"old\": 1, // 原注释\r\n}\r\n",
            "{\r\n  \"old\": 1, // 原注释\r\n  \"new\": null,\r\n}\r\n",
        ),
    ] {
        write_private(&path, original);
        let mut store = ConfigStore::load(path.clone()).unwrap();
        store.replace("new", Value::Null).unwrap();
        assert_eq!(fs::read_to_string(&path).unwrap(), expected);
    }
}

#[test]
fn rejects_nonregular_paths_and_nonprivate_modes() {
    // 普通文件的权限必须精确匹配，目录、符号链接、FIFO 和套接字都拒绝。
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("config.jsonc");
    write_private(&path, "{}");
    for mode in [0o644, 0o400, 0o700, 0o1600] {
        fs::set_permissions(&path, fs::Permissions::from_mode(mode)).unwrap();
        assert!(
            ConfigStore::load(path.clone())
                .err()
                .unwrap()
                .contains("0600")
        );
    }
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
    assert!(ConfigStore::load(dir.path().to_path_buf()).is_err());
    let link = dir.path().join("symlink");
    symlink(&path, &link).unwrap();
    assert!(ConfigStore::load(link).is_err());
    let dangling = dir.path().join("dangling");
    symlink(dir.path().join("missing"), &dangling).unwrap();
    assert!(ConfigStore::load(dangling).is_err());
    let socket = dir.path().join("socket");
    let _listener = UnixListener::bind(&socket).unwrap();
    assert!(ConfigStore::load(socket).is_err());
    let fifo = dir.path().join("fifo");
    assert!(
        std::process::Command::new("mkfifo")
            .args(["-m", "600"])
            .arg(&fifo)
            .status()
            .unwrap()
            .success()
    );
    assert!(ConfigStore::load(fifo).is_err());
}

#[test]
fn rejects_malformed_or_ambiguous_documents() {
    // 拒绝非法 UTF-8、缺失根对象、扩展 JSON5 语法和有歧义的重复区段。
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("config.jsonc");
    for bytes in [
        b"\xff".as_slice(),
        b"// only a comment",
        b"[]",
        b"{terminal: true}",
        b"{\"terminal\": true \"desktop\": false}",
        br#"{"terminal": true, "termi\u006eal": false}"#,
        b"{} {}",
    ] {
        write_private(&path, bytes);
        assert!(
            ConfigStore::load(path.clone()).is_err(),
            "accepted {bytes:?}"
        );
    }
}

#[test]
fn limits_document_size_and_nesting_before_persisting() {
    // 同时覆盖读取和写入上限，失败不能修改内存或磁盘。
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("config.jsonc");
    write_private(&path, vec![b' '; MAX_BYTES + 1]);
    assert!(
        ConfigStore::load(path.clone())
            .err()
            .unwrap()
            .contains("1 MiB")
    );
    write_private(
        &path,
        format!(
            "{{\"value\": {}null{}}}",
            "[".repeat(MAX_DEPTH),
            "]".repeat(MAX_DEPTH)
        ),
    );
    assert!(
        ConfigStore::load(path.clone())
            .err()
            .unwrap()
            .contains("64 levels")
    );

    let original = "{\"value\": 1}\n";
    write_private(&path, original);
    let mut store = ConfigStore::load(path.clone()).unwrap();
    let large = Value::String("x".repeat(MAX_BYTES));
    assert!(store.replace("value", large).unwrap_err().contains("1 MiB"));
    let deep = (0..MAX_DEPTH).fold(Value::Null, |value, _| Value::Array(vec![value]));
    assert!(
        store
            .replace("value", deep)
            .unwrap_err()
            .contains("64 levels")
    );
    assert_eq!(store.section("value"), Some(&json!(1)));
    assert_eq!(fs::read_to_string(&path).unwrap(), original);
}

#[test]
fn refreshes_other_sections_and_rejects_permission_changes() {
    // 写入前重读磁盘保留已完成的手动编辑，权限后来变更时写入失败且保留快照。
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("config.jsonc");
    write_private(&path, r#"{"terminal": 1, "desktop": 2}"#);
    let mut store = ConfigStore::load(path.clone()).unwrap();
    write_private(&path, r#"{"terminal": 1, "desktop": 3 /* 手动编辑 */}"#);
    store.replace("terminal", json!(4)).unwrap();
    assert_eq!(store.section("desktop"), Some(&json!(3)));
    let updated = r#"{"terminal": 4, "desktop": 3 /* 手动编辑 */}"#;
    assert_eq!(fs::read_to_string(&path).unwrap(), updated);

    fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).unwrap();
    assert!(store.replace("terminal", json!(5)).is_err());
    assert_eq!(store.section("terminal"), Some(&json!(4)));
    assert_eq!(fs::read_to_string(path).unwrap(), updated);
}
