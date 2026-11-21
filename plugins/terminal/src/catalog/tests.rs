use super::*;
use crate::tests::TempDir;

#[test]
fn resolves_catalog_root_in_priority_order() {
    // 注入环境变量而不修改进程环境，验证显式目录、应用变量、XDG 和 HOME 的优先级。
    let absolute = env::temp_dir().join("deskwaifu-user-home");
    let xdg = absolute.join("custom data home");
    let configured = |name: &str| match name {
        "DESKWAIFU_MODEL_DIR" => Some("configured models".into()),
        "XDG_DATA_HOME" => Some(xdg.clone().into_os_string()),
        "HOME" => Some(absolute.clone().into_os_string()),
        _ => None,
    };
    assert_eq!(
        catalog_root(Some("explicit"), configured).unwrap(),
        ("explicit".into(), false)
    );
    assert_eq!(
        catalog_root(None, configured).unwrap(),
        ("configured models".into(), false)
    );
    assert_eq!(
        catalog_root(None, |name| if name == "DESKWAIFU_MODEL_DIR" {
            Some(OsString::new())
        } else {
            configured(name)
        })
        .unwrap(),
        (xdg.join("deskwaifu/models"), true),
    );
    assert_eq!(
        catalog_root(None, |name| match name {
            "XDG_DATA_HOME" => Some("relative".into()),
            "HOME" => configured(name),
            _ => None,
        })
        .unwrap(),
        (absolute.join(".local/share/deskwaifu/models"), true),
    );
    assert!(catalog_root(None, |_| None).is_err());
}

#[test]
fn handles_missing_default_but_rejects_invalid_explicit_root() {
    // 用未创建的目录验证默认路径首次使用为空，并验证显式路径及普通文件出错。
    let temp = TempDir::new();
    let missing = temp.0.join("missing");
    let file = temp.0.join("file.model3.json");
    fs::write(&file, "test").unwrap();
    assert!(
        discover(&missing, true, MAX_DEPTH, MAX_ENTRIES)
            .unwrap()
            .is_empty()
    );
    assert!(discover(&missing, false, MAX_DEPTH, MAX_ENTRIES).is_err());
    assert!(discover(&file, false, MAX_DEPTH, MAX_ENTRIES).is_err());
}

#[test]
fn bounds_depth_and_counts_all_directory_entries() {
    // 缩小扫描限额，在临时目录验证深度边界，以及非模型项也计入目录项上限。
    let temp = TempDir::new();
    fs::create_dir_all(temp.0.join("one/two")).unwrap();
    fs::write(temp.0.join("README.txt"), "test").unwrap();
    fs::write(temp.0.join("one/model.model3.json"), "test").unwrap();
    fs::write(temp.0.join("one/two/deep.model3.json"), "test").unwrap();

    let models = discover(&temp.0, false, 1, 4).unwrap();
    let canonical = fs::canonicalize(&temp.0).unwrap();
    assert_eq!(
        models,
        [canonical.join("one/model.model3.json").to_str().unwrap()]
    );
    assert_eq!(
        discover(&temp.0, false, 1, 3),
        Err("terminal catalog: entry limit (3) exceeded".into())
    );
}

#[test]
fn enforces_the_ipc_frame_limit_after_json_escaping() {
    // 用包含引号、反斜杠和换行的路径填满 IPC 响应，验证多一个字节即返回目录查询错误。
    let escaped_path = "models/引号\"\\\n";
    let host_frame = |output: &str| {
        serde_json::to_vec(&serde_json::json!({"status": "ok", "output": output})).unwrap()
    };
    let mut catalog = Catalog {
        models: vec![format!("{escaped_path}.model3.json")],
    };
    let overhead = host_frame(&serialize(&catalog).unwrap()).len();
    catalog.models[0] = format!(
        "{escaped_path}{}.model3.json",
        "x".repeat(MAX_FRAME_BYTES - overhead)
    );
    let output = serialize(&catalog).unwrap();
    assert_eq!(host_frame(&output).len(), MAX_FRAME_BYTES);
    catalog.models[0].push('x');
    assert!(
        serialize(&catalog)
            .unwrap_err()
            .contains("response exceeds 64 KiB")
    );
}

#[cfg(unix)]
#[test]
fn skips_symlinks_special_files_and_non_utf8_paths() {
    use std::os::unix::ffi::OsStringExt;
    use std::os::unix::fs::symlink;
    use std::os::unix::net::UnixListener;

    // 构造指向目录外的链接、循环链接、Unix socket 和非 UTF-8 文件，验证均不进入结果。
    let temp = TempDir::new();
    let outside = TempDir::new();
    let outside_model = outside.0.join("outside.model3.json");
    fs::write(&outside_model, "test").unwrap();
    symlink(&outside.0, temp.0.join("outside")).unwrap();
    symlink(&outside_model, temp.0.join("linked.model3.json")).unwrap();
    symlink(&temp.0, temp.0.join("loop")).unwrap();
    let _socket = UnixListener::bind(temp.0.join("socket.model3.json")).unwrap();
    fs::write(
        temp.0
            .join(OsString::from_vec(b"\xff.model3.json".to_vec())),
        "test",
    )
    .unwrap();
    assert!(
        discover(&temp.0, false, MAX_DEPTH, MAX_ENTRIES)
            .unwrap()
            .is_empty()
    );
}
