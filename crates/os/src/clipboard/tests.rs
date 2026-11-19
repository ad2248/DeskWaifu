use super::*;
use std::{
    fs,
    os::unix::fs::PermissionsExt,
    path::PathBuf,
    sync::atomic::{AtomicUsize, Ordering},
};

struct Fixtures {
    directory: PathBuf,
    clipboard: Clipboard,
}

impl Fixtures {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let directory = std::env::temp_dir().join(format!(
            "deskwaifu-clipboard-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed),
        ));
        fs::create_dir(&directory).unwrap();
        Self {
            clipboard: Clipboard {
                backend: Backend {
                    copy: directory.join("copy").into_os_string(),
                    paste: directory.join("paste").into_os_string(),
                    timeout: OPERATION_TIMEOUT,
                },
                ..Clipboard::default()
            },
            directory,
        }
    }

    fn script(&self, name: &str, script: &str) {
        let path = self.directory.join(name);
        // 替换路径而非原地修改，允许测试更新仍在退出中的脚本。
        let new = path.with_extension("new");
        fs::write(
            &new,
            format!("#!/bin/sh\nset -eu\ncd -- \"$(dirname -- \"$0\")\"\n{script}\n"),
        )
        .unwrap();
        fs::set_permissions(&new, fs::Permissions::from_mode(0o700)).unwrap();
        fs::rename(new, path).unwrap();
    }

    async fn child_started(&self) {
        tokio::time::timeout(Duration::from_secs(3), async {
            while fs::read_to_string(self.directory.join("pid"))
                .ok()
                .and_then(|text| text.trim().parse::<u32>().ok())
                .is_none()
            {
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        })
        .await
        .expect("clipboard child did not start");
    }
}

impl Drop for Fixtures {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.directory);
    }
}

#[tokio::test]
async fn reads_none_for_empty_selection_and_for_nontext_mime() {
    let fixture = Fixtures::new();
    fixture.script("paste", "echo 'Nothing is copied' >&2; exit 1");
    assert_eq!(fixture.clipboard.read().await, Ok(None));
    fixture.script(
        "paste",
        "echo 'Clipboard content is not available as requested type \"text\"' >&2; exit 1",
    );
    assert_eq!(fixture.clipboard.read().await, Ok(None));
    fixture.script(
        "paste",
        "echo 'Failed to connect to a Wayland server' >&2; exit 1",
    );
    assert_eq!(fixture.clipboard.read().await, Err(OsError::Backend));
}

#[tokio::test]
async fn write_and_independent_read_preserve_text_and_empty_string() {
    let fixture = Fixtures::new();
    fixture.script(
        "copy",
        "[ \"$1\" = --type ]; [ \"$2\" = 'text/plain;charset=utf-8' ]; exec cat >selection",
    );
    fixture.script(
        "paste",
        "[ \"$1\" = --no-newline ]; [ \"$2\" = --type ]; [ \"$3\" = text ]; exec cat selection",
    );
    for text in [" \t中文\n下一行 \n\n", ""] {
        assert_eq!(fixture.clipboard.write(text.into()).await, Ok(()));
        assert_eq!(fixture.clipboard.read().await, Ok(Some(text.into())));
    }
}

#[tokio::test]
#[ignore = "requires an isolated Wayland compositor and wl-clipboard; overwrites its clipboard"]
async fn real_wayland_selection_preserves_text_and_empty_string() {
    let clipboard = Clipboard::default();
    assert_eq!(clipboard.read().await, Ok(None));
    for text in [" \t中文\n下一行 \n\n", ""] {
        assert_eq!(clipboard.write(text.into()).await, Ok(()));
        let output = tokio::time::timeout(
            OPERATION_TIMEOUT,
            Command::new("wl-paste")
                .args(["--no-newline", "--type", "text"])
                .kill_on_drop(true)
                .output(),
        )
        .await
        .expect("independent wl-paste timed out")
        .expect("failed to run independent wl-paste");
        assert!(output.status.success(), "{:?}", output.stderr);
        assert_eq!(output.stdout, text.as_bytes());
        assert_eq!(clipboard.read().await, Ok(Some(text.into())));
    }
}

#[tokio::test]
async fn bounds_input_and_output_and_rejects_invalid_utf8() {
    let fixture = Fixtures::new();
    fixture.script("paste", "exec head -c 1048576 /dev/zero");
    assert_eq!(
        fixture.clipboard.read().await.unwrap().unwrap().len(),
        MAX_TEXT,
    );
    fixture.script("paste", "exec head -c 1048577 /dev/zero");
    assert_eq!(fixture.clipboard.read().await, Err(OsError::TooLarge));
    fixture.script("paste", "printf '\\377'");
    assert_eq!(fixture.clipboard.read().await, Err(OsError::InvalidText));
    // wl-paste 可能忽略其读取子进程的退出码，但错误诊断仍会写入 stderr。
    fixture.script("paste", "printf partial; echo 'cat: read error' >&2");
    assert_eq!(fixture.clipboard.read().await, Err(OsError::Backend));
    assert_eq!(
        fixture.clipboard.write("x".repeat(MAX_TEXT + 1)).await,
        Err(ClipboardWriteError::Failed(OsError::TooLarge)),
    );
}

#[tokio::test]
async fn write_distinguishes_preflight_failure_from_unconfirmed_selection() {
    let fixture = Fixtures::new();
    assert_eq!(
        fixture.clipboard.write("new".into()).await,
        Err(ClipboardWriteError::Failed(OsError::Unavailable)),
    );
    fixture.script("copy", "cat >/dev/null; exit 1");
    assert_eq!(
        fixture.clipboard.write("new".into()).await,
        Err(ClipboardWriteError::Unknown(OsError::Backend)),
    );
    fixture.script("copy", "exec cat >/dev/null");
    fixture.script("paste", "printf old");
    assert_eq!(
        fixture.clipboard.write("new".into()).await,
        Err(ClipboardWriteError::Unknown(OsError::Backend)),
    );
}

#[tokio::test]
async fn timed_out_process_is_reaped_before_returning() {
    let mut fixture = Fixtures::new();
    fixture.clipboard.backend.timeout = Duration::from_secs(1);
    fixture.script("paste", "echo $$ >pid; exec sleep 30");
    assert_eq!(fixture.clipboard.read().await, Err(OsError::Timeout));
    fixture.script(
        "paste",
        "if kill -0 \"$(cat pid)\" 2>/dev/null; then exit 1; fi; printf next",
    );
    assert_eq!(fixture.clipboard.read().await, Ok(Some("next".into())));
}

#[tokio::test]
async fn cancellation_reaps_before_the_next_read() {
    let fixture = Arc::new(Fixtures::new());
    fixture.script("paste", "echo $$ >pid; exec sleep 30");
    let other = Arc::clone(&fixture);
    let task = tokio::spawn(async move { other.clipboard.read().await });
    fixture.child_started().await;
    fixture.script(
        "paste",
        "if kill -0 \"$(cat pid)\" 2>/dev/null; then exit 1; fi; printf next",
    );
    task.abort();
    assert!(task.await.unwrap_err().is_cancelled());
    assert_eq!(fixture.clipboard.read().await, Ok(Some("next".into())));
}

#[tokio::test]
async fn shutdown_cancels_a_running_command_and_rejects_future_operations() {
    let fixture = Arc::new(Fixtures::new());
    fixture.script("copy", "cat >/dev/null; echo $$ >pid; exec sleep 30");
    let other = Arc::clone(&fixture);
    let task = tokio::spawn(async move { other.clipboard.write("text".into()).await });
    fixture.child_started().await;
    fixture.clipboard.shutdown().await;
    assert_eq!(
        task.await.unwrap(),
        Err(ClipboardWriteError::Unknown(OsError::Unavailable))
    );
    assert_eq!(fixture.clipboard.read().await, Err(OsError::Unavailable));
    assert_eq!(
        fixture.clipboard.write("text".into()).await,
        Err(ClipboardWriteError::Failed(OsError::Unavailable)),
    );
}
