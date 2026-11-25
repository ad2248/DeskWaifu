use super::*;
use crate::tests::TempDir;

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
