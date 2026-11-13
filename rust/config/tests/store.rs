use deskwaifu_config::settings::{ConfigDocument, Settings};
use deskwaifu_config::store::Store;
use std::fs;
use std::os::unix::fs::{symlink, PermissionsExt};
use std::path::Path;

const MAX_FILE_SIZE: u64 = 4 * 1024 * 1024;

fn write(path: &Path, text: &str) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, text).unwrap();
}

fn empty() -> ConfigDocument {
    ConfigDocument::parse("{}", true).unwrap()
}

fn mode(path: &Path) -> u32 {
    fs::metadata(path).unwrap().permissions().mode() & 0o777
}

/// A privileged process bypasses directory and file mode bits.
fn running_as_root() -> bool {
    fs::metadata("/proc/self").is_ok_and(|info| {
        use std::os::unix::fs::MetadataExt;
        info.uid() == 0
    })
}

#[test]
fn missing_file_reads_as_empty_and_commit_creates_private_file() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    assert_eq!(store.path(), root.path().join("deskwaifu/config.json"));
    let snapshot = store.read().unwrap();
    assert!(snapshot.values.is_empty());
    assert_eq!(snapshot.document, "{}\n");
    assert!(!store.path().exists());
    store
        .commit("{\"model_scale\":1}", &snapshot, false)
        .unwrap();
    assert_eq!(mode(store.path()), 0o600);
    let saved = store.read().unwrap();
    assert_eq!(saved.values.len(), 1);
    assert_eq!(saved.values["model_scale"], "1");
    let content: serde_json::Value =
        serde_json::from_slice(&fs::read(store.path()).unwrap()).unwrap();
    assert_eq!(content["model_scale"], 1);
}

#[test]
fn missing_directories_are_created_private() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path().join("fresh"));
    let snapshot = store.read().unwrap();
    store.commit("{\"model_x\":1}", &snapshot, false).unwrap();
    assert_eq!(mode(&root.path().join("fresh/deskwaifu")), 0o700);
    assert_eq!(mode(store.path()), 0o600);
}

#[test]
fn round_trip_preserves_unknown_keys_but_not_comments() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(
        store.path(),
        "{\n // keep this comment\n \"other\": {\"model_x\": 9},\n \"model_x\": 1,\n \
         \"model_region\": {\"x\":100,\"width\":800,\"height\":600}\n}\n",
    );
    let before = store.read().unwrap();
    assert_eq!(before.values["other"], "{\"model_x\":9}");
    let mut settings = Settings::default();
    Settings::decode(&before, &mut settings).unwrap();
    assert_eq!(settings.model_x, 1.0);
    settings.model_x = 2.5;
    let document = settings.encode().unwrap();
    let saved = store.commit_settings(&document, &before, false).unwrap();
    assert_eq!(saved.values["model_x"], "2.5");
    assert_eq!(saved.values["other"], "{\"model_x\":9}");
    assert!(saved.values.contains_key("model_region"));
    let text = fs::read_to_string(store.path()).unwrap();
    assert!(!text.contains("// keep this comment"));
    let restored = store.read().unwrap();
    assert_eq!(restored.values, saved.values);
    let mut decoded = Settings::default();
    Settings::decode(&restored, &mut decoded).unwrap();
    assert_eq!(decoded, settings);
    assert_eq!(decoded.unknown_fields["other"], "{\"model_x\":9}");
}

#[test]
fn commit_replaces_the_whole_document() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(
        store.path(),
        "{\n \"other\": {\"model_x\": 9},\n \"model_x\": 1, // dropped\n \
         \"model_region\": {\"x\":100,\"width\":800,\"height\":600}\n}\n",
    );
    let before = store.read().unwrap();
    let saved = store
        .commit("{\"model_x\":2.5,\"model_y\":-1}", &before, false)
        .unwrap();
    assert_eq!(saved.values.len(), 2);
    assert_eq!(saved.values["model_x"], "2.5");
    assert_eq!(saved.values["model_y"], "-1");
    assert!(!saved.values.contains_key("other"));
    assert!(!saved.values.contains_key("model_region"));
    assert!(!saved.document.contains("// dropped"));
    assert_eq!(store.read().unwrap().values, saved.values);
    let emptied = store.commit("{}", &saved, false).unwrap();
    assert!(emptied.values.is_empty());
}

#[test]
fn unrelated_change_conflicts_and_overwrite_skips_the_comparison() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(store.path(), "{\"model_x\":1,\"other\":1}");
    let expected = store.read().unwrap();
    // An external edit that does not touch the saved field must still conflict.
    write(store.path(), "{\"model_x\":1,\"other\":2}");
    let error = store
        .commit("{\"model_x\":3,\"other\":1}", &expected, false)
        .unwrap_err();
    assert!(error.to_string().contains("conflict"), "{error}");
    assert_eq!(
        fs::read_to_string(store.path()).unwrap(),
        "{\"model_x\":1,\"other\":2}"
    );
    let saved = store
        .commit("{\"model_x\":3,\"other\":1}", &expected, true)
        .unwrap();
    assert_eq!(saved.values["model_x"], "3");
    assert_eq!(saved.values["other"], "1");
    assert_eq!(store.read().unwrap().values, saved.values);
}

#[test]
fn existing_permissions_are_preserved_and_private_mode_forces_0600() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(store.path(), "{\"model_x\":1}");
    fs::set_permissions(store.path(), fs::Permissions::from_mode(0o640)).unwrap();
    let snapshot = store.read().unwrap();
    store.commit("{\"model_x\":2}", &snapshot, false).unwrap();
    assert_eq!(mode(store.path()), 0o640);

    let private = Store::new_private(root.path());
    let snapshot = private.read().unwrap();
    private.commit("{\"model_x\":3}", &snapshot, false).unwrap();
    assert_eq!(mode(private.path()), 0o600);
    assert_eq!(private.read().unwrap().values["model_x"], "3");

    let root = tempfile::tempdir().unwrap();
    let private = Store::new_private(root.path());
    let snapshot = private.read().unwrap();
    private.commit("{}", &snapshot, false).unwrap();
    assert_eq!(mode(private.path()), 0o600);
}

#[test]
fn read_only_directory_commit_failure_keeps_content_and_snapshot() {
    if running_as_root() {
        eprintln!("skipping read-only directory injection for a privileged process");
        return;
    }
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(store.path(), "{\"model_x\":6}");
    let snapshot = store.read().unwrap();
    let parent = store.path().parent().unwrap().to_owned();
    // Without write permission on the directory no temporary file can be created.
    fs::set_permissions(&parent, fs::Permissions::from_mode(0o500)).unwrap();
    let error = store
        .commit("{\"model_x\":99}", &snapshot, false)
        .unwrap_err();
    assert!(!error.to_string().is_empty());
    fs::set_permissions(&parent, fs::Permissions::from_mode(0o700)).unwrap();
    // The rejected commit kept the file, the caller's snapshot and the temp namespace.
    assert_eq!(fs::read_to_string(store.path()).unwrap(), "{\"model_x\":6}");
    assert_eq!(store.read().unwrap(), snapshot);
    assert_eq!(fs::read_dir(&parent).unwrap().count(), 1);
    store.commit("{\"model_x\":6}", &snapshot, false).unwrap();
    assert_eq!(store.read().unwrap().values["model_x"], "6");
}

#[test]
fn path_component_that_is_a_regular_file_fails_the_commit() {
    let root = tempfile::tempdir().unwrap();
    let blocker = root.path().join("deskwaifu");
    fs::write(&blocker, b"not a directory").unwrap();
    let store = Store::new(root.path());
    assert!(store.read().is_err());
    assert!(store.commit("{}", &empty(), true).is_err());
    assert_eq!(fs::read(&blocker).unwrap(), b"not a directory");
}

#[test]
fn invalid_document_is_rejected_and_not_replaced() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(store.path(), "{broken");
    assert!(store.read().is_err());
    // Commit re-reads and parses the file first, so even overwrite cannot replace
    // a corrupt document with a whole-document write.
    assert!(store.commit("{\"model_x\":0}", &empty(), false).is_err());
    assert!(store.commit("{\"model_x\":0}", &empty(), true).is_err());
    assert_eq!(fs::read_to_string(store.path()).unwrap(), "{broken");

    write(store.path(), "{}");
    let snapshot = store.read().unwrap();
    // `commit` validates JSON syntax only, as `ConfigStore::Commit` does.
    store
        .commit("{\"render_device\":\"intel\"}", &snapshot, false)
        .unwrap();
    let snapshot = store.read().unwrap();
    // The settings path additionally runs Settings::decode before writing.
    assert!(store
        .commit_settings("{\"render_device\":\"intel\"}", &snapshot, false)
        .is_err());
    assert_eq!(
        fs::read_to_string(store.path()).unwrap(),
        "{\"render_device\":\"intel\"}"
    );
    store
        .commit_settings("{\"render_device\":\"0000:00:02.0\"}", &snapshot, false)
        .unwrap();
    assert_eq!(
        store.read().unwrap().values["render_device"],
        "\"0000:00:02.0\""
    );
}

#[test]
fn commit_rejects_oversized_documents_before_replacing_the_current_file() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(store.path(), "{\"model_x\":1}");
    let snapshot = store.read().unwrap();
    let oversized = format!("{{\"other\":\"{}\"}}", "x".repeat(MAX_FILE_SIZE as usize));
    assert!(oversized.len() as u64 > MAX_FILE_SIZE);
    for overwrite in [false, true] {
        let error = store.commit(&oversized, &snapshot, overwrite).unwrap_err();
        assert!(error.to_string().contains("4 MiB"), "{error}");
        assert_eq!(fs::read_to_string(store.path()).unwrap(), "{\"model_x\":1}");
        assert_eq!(store.read().unwrap(), snapshot);
    }
}

#[test]
fn oversized_file_is_rejected_and_unchanged() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    let data = format!("{{}}{}", " ".repeat(MAX_FILE_SIZE as usize - 1));
    assert_eq!(data.len() as u64, MAX_FILE_SIZE + 1);
    write(store.path(), &data);
    assert!(store.read().is_err());
    let error = store.commit("{\"model_x\":0}", &empty(), true).unwrap_err();
    assert!(error.to_string().contains("4 MiB"), "{error}");
    assert_eq!(fs::read_to_string(store.path()).unwrap(), data);
}

#[test]
fn size_limit_is_inclusive() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    let data = format!(
        "{{\"model_x\":1}}{}",
        " ".repeat(MAX_FILE_SIZE as usize - 13)
    );
    assert_eq!(data.len() as u64, MAX_FILE_SIZE);
    write(store.path(), &data);
    let snapshot = store.read().unwrap();
    assert_eq!(snapshot.values["model_x"], "1");
    store.commit("{\"model_x\":2}", &snapshot, false).unwrap();
}

#[test]
fn non_regular_target_is_rejected() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    fs::create_dir_all(store.path()).unwrap();
    let error = store.read().unwrap_err();
    assert!(error.to_string().contains("regular file"), "{error}");
    assert!(store.commit("{}", &empty(), true).is_err());
    assert!(fs::metadata(store.path()).unwrap().is_dir());
}

#[test]
fn symlinks_are_followed_on_save_and_dangling_links_are_rejected() {
    let target = tempfile::tempdir().unwrap();
    let target_store = Store::new(target.path());
    write(target_store.path(), "{}");

    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    fs::create_dir_all(store.path().parent().unwrap()).unwrap();
    symlink(target_store.path(), store.path()).unwrap();
    let before = store.read().unwrap();
    store.commit("{\"model_x\":7}", &before, false).unwrap();
    // The symlink survives and its target received the document.
    assert!(fs::symlink_metadata(store.path()).unwrap().is_symlink());
    assert_eq!(target_store.read().unwrap().values["model_x"], "7");

    // A dangling symlink is refused instead of being replaced by a regular file.
    let dangling_root = tempfile::tempdir().unwrap();
    let dangling = Store::new(dangling_root.path());
    fs::create_dir_all(dangling.path().parent().unwrap()).unwrap();
    symlink("missing.json", dangling.path()).unwrap();
    assert!(dangling.read().is_err());
    assert!(dangling.commit("{\"model_x\":0}", &before, false).is_err());
    assert!(fs::symlink_metadata(dangling.path()).unwrap().is_symlink());
}

#[test]
fn change_between_read_and_commit_is_detected() {
    let root = tempfile::tempdir().unwrap();
    let store = Store::new(root.path());
    write(store.path(), "{\"model_x\":1}");
    let snapshot = store.read().unwrap();
    // The file is edited after the caller took its snapshot.
    write(store.path(), "{\"model_x\":2}");
    let error = store
        .commit("{\"model_x\":3}", &snapshot, false)
        .unwrap_err();
    assert!(error.to_string().contains("conflict"), "{error}");
    assert_eq!(fs::read_to_string(store.path()).unwrap(), "{\"model_x\":2}");
    // Re-reading accepts the external value, and a matching snapshot commits.
    let current = store.read().unwrap();
    store.commit("{\"model_x\":3}", &current, false).unwrap();
    assert_eq!(store.read().unwrap().values["model_x"], "3");
}
