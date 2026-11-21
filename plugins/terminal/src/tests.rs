use super::*;
use deskwaifu_runtime::Runtime;
use serde_json::{Value, json};
use std::fs;
use std::io::ErrorKind;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

pub(crate) struct TempDir(pub(crate) PathBuf);

impl TempDir {
    pub(crate) fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        loop {
            let path = std::env::temp_dir().join(format!(
                "deskwaifu-terminal-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed),
            ));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("failed to create test directory: {error}"),
            }
        }
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn argv(args: &[&str]) -> Vec<String> {
    args.iter().map(|arg| (*arg).to_owned()).collect()
}

fn runtime() -> Runtime {
    let mut runtime = Runtime::new(16);
    runtime.add(TerminalPlugin::default()).unwrap();
    runtime.start().unwrap();
    runtime
}

#[test]
fn validates_arguments_before_backend_dispatch() {
    // 通过宿主路由发送形状错误的参数，验证后端可用性不影响参数错误。
    let mut runtime = runtime();
    for args in [
        &[][..],
        &["status", "extra"],
        &["catalog", ""],
        &["catalog", "a\0b"],
        &["catalog", "a", "b"],
        &["motions", "extra"],
        &["expressions", "extra"],
        &["outputs", "extra"],
        &["opacity", "NaN"],
        &["opacity", "1.1"],
        &["crop", "0", "0", "0", "1"],
        &["crop", "0", "0", "1"],
        &["output", ""],
        &["output", "a", "b"],
        &["expression"],
        &["expression", ""],
        &["expression", "a\0b"],
        &["expression", "a", "b"],
        &["load"],
        &["load", ""],
        &["load", "a\0b"],
        &["load", "a", "b"],
        &["show", "extra"],
        &["hide", "extra"],
        &["motion", "Idle"],
        &["motion", "Idle", "1", "extra"],
        &["motion", "", "0"],
        &["motion", "a\0b", "0"],
        &["motion", "Idle", "-1"],
        &["motion", "Idle", "bad"],
        &["motion", "Idle", "4294967296"],
    ] {
        let command = [&["terminal"], args].concat();
        assert_eq!(
            runtime.dispatch_command(&argv(&command)),
            Err(INVALID_ARGUMENTS.into()),
            "{args:?}",
        );
    }
    runtime.stop().unwrap();
}

#[test]
fn runtime_exposes_terminal_owned_model_tab() {
    // 通过宿主查询已注册标签页，核对稳定 ID 和按钮, 再通过路由调用只读操作。
    let mut runtime = runtime();
    let text = runtime.dispatch_command(&argv(&["panel-tabs"])).unwrap();
    let panel: serde_json::Value = serde_json::from_str(&text).unwrap();
    let tabs = panel["tabs"].as_array().unwrap();
    assert_eq!(tabs.len(), 1);
    assert_eq!(tabs[0]["plugin"], "terminal");
    assert_eq!(tabs[0]["id"], "models");
    assert_eq!(
        tabs[0]["sections"][0]["actions"][0]["args"],
        serde_json::json!(["terminal", "show"])
    );
    runtime.stop().unwrap();
    assert!(runtime.dispatch_command(&argv(&["panel-tabs"])).is_err());
}

#[test]
fn help_lists_p1_commands() {
    // 从宿主的帮助文本读取插件登记信息，并确认后续子命令未启用。
    let mut runtime = runtime();
    let help = runtime.dispatch_command(&argv(&["help"])).unwrap();
    for command in [
        "status",
        "catalog [DIRECTORY]",
        "load PATH",
        "show",
        "hide",
        "motion GROUP INDEX",
        "motions",
        "expressions",
        "expression NAME",
        "outputs",
        "output [NAME]",
    ] {
        assert!(help.contains(&format!("terminal {command}")), "{help}");
    }
    assert_eq!(
        runtime.dispatch_command(&argv(&["terminal", "unknown"])),
        Err("unknown_command".into()),
    );
    runtime.stop().unwrap();
}

#[test]
fn catalog_returns_sorted_paths_through_runtime_dispatch() {
    // 在临时目录创建普通文件、子目录与后缀相近的文件，通过宿主查询验证格式与排序。
    let temp = TempDir::new();
    let nested = "子目录";
    fs::create_dir(temp.0.join(nested)).unwrap();
    for path in [
        "B.model3.json",
        "A model.model3.json",
        "other.model3.json.bak",
    ] {
        fs::write(temp.0.join(path), "not parsed").unwrap();
    }
    fs::write(temp.0.join(nested).join("C.model3.json"), "not parsed").unwrap();
    fs::create_dir(temp.0.join("directory.model3.json")).unwrap();

    let mut runtime = runtime();
    let result = runtime
        .dispatch_command(&argv(&["terminal", "catalog", temp.0.to_str().unwrap()]))
        .unwrap();
    let actual: Value = serde_json::from_str(&result).unwrap();
    let canonical = fs::canonicalize(&temp.0).unwrap();
    assert_eq!(
        actual,
        json!({"models": [
            canonical.join("A model.model3.json").to_str().unwrap(),
            canonical.join("B.model3.json").to_str().unwrap(),
            canonical.join(nested).join("C.model3.json").to_str().unwrap(),
        ]})
    );
    runtime.stop().unwrap();
}

#[cfg(not(all(target_os = "linux", feature = "native")))]
#[test]
fn default_build_reports_unavailable() {
    // 无 SDK 时插件仍能启动及查询状态，所有变更命令都明确拒绝受理。
    let mut runtime = runtime();
    for command in ["status", "motions", "expressions"] {
        let result = runtime
            .dispatch_command(&argv(&["terminal", command]))
            .unwrap();
        let result: Value = serde_json::from_str(&result).unwrap();
        assert_eq!(result["snapshot"]["state"], "unavailable");
        assert_eq!(result["snapshot"]["error"], UNAVAILABLE);
        assert_eq!(result["snapshot"]["pending"], json!([]));
        assert_eq!(result["snapshot"]["motions"], json!([]));
        assert_eq!(result["snapshot"]["expressions"], json!([]));
        assert!(result.get("accepted_operation_id").is_none());
    }
    for command in [
        &["terminal", "show"][..],
        &["terminal", "hide"],
        &["terminal", "load", "model.model3.json"],
        &["terminal", "motion", "Idle", "0"],
        &["terminal", "expression", "smile"],
    ] {
        assert_eq!(
            runtime.dispatch_command(&argv(command)),
            Err(UNAVAILABLE.into())
        );
    }
    runtime.stop().unwrap();
}
