//! Private credential-file storage.
//!
//! This slice does not provide sound concurrency-conflict protection: concurrent
//! writers can overwrite one another. Callers must not treat it as the complete
//! ConfigStore semantics described by the application contract.

pub mod settings;
pub mod store;

use serde::de::{MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};
use serde_json::{Map, Number, Value};
use std::fmt;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;

const MAX_FILE_SIZE: u64 = 4 * 1024 * 1024;

/// Stores a named credential in `<root>/plugin/<plugin>/config.json`.
pub struct CredentialStore {
    path: PathBuf,
}

impl CredentialStore {
    /// Construct a store using an explicit config root and trusted plugin name.
    /// The caller must supply a single directory name, not an untrusted path.
    pub fn new(config_root: impl Into<PathBuf>, plugin_name: &str) -> Self {
        Self {
            path: config_root
                .into()
                .join("plugin")
                .join(plugin_name)
                .join("config.json"),
        }
    }

    /// Return the backing file path.
    pub fn path(&self) -> &std::path::Path {
        &self.path
    }

    /// Read and validate the JSON object without creating an absent file.
    pub fn read(&self) -> Result<(), StoreError> {
        let path = self.resolved()?;
        read_object(&path).map(|_| ())
    }

    /// Read one credential value without creating an absent file.
    pub fn get(&self, name: &str) -> Result<Option<Value>, StoreError> {
        let path = self.resolved()?;
        Ok(read_object(&path)?.remove(name))
    }

    /// Set or clear a credential. Empty values remove the named field.
    pub fn set(&self, name: &str, credential: &str) -> Result<(), StoreError> {
        validate_credential(credential)?;
        // Resolve before writing so a credential file that is a symbolic link
        // keeps the link: `rename` onto the link path would replace the link
        // itself with a regular file and leave the real target stale. This is
        // the same resolution `store::Store` uses, so both paths agree.
        let path = self.resolved()?;
        let mut object = read_object(&path)?;
        if credential.is_empty() {
            object.remove(name);
        } else {
            object.insert(name.to_owned(), Value::String(credential.to_owned()));
        }
        let data = serde_json::to_vec(&Value::Object(object)).map_err(|_| StoreError)?;
        if data.len() as u64 > MAX_FILE_SIZE {
            return Err(StoreError);
        }
        let parent = path.parent().ok_or(StoreError)?;
        fs::create_dir_all(parent).map_err(|_| StoreError)?;
        let mut temp = tempfile::Builder::new()
            .prefix(".config-")
            .tempfile_in(parent)
            .map_err(|_| StoreError)?;
        temp.as_file()
            .set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| StoreError)?;
        temp.write_all(&data).map_err(|_| StoreError)?;
        temp.as_file().sync_all().map_err(|_| StoreError)?;
        temp.persist(&path).map_err(|_| StoreError)?;
        Ok(())
    }

    /// The backing path with symbolic links followed; a dangling link is an
    /// error rather than a path that would be replaced.
    fn resolved(&self) -> Result<PathBuf, StoreError> {
        store::resolve(&self.path).map_err(|_| StoreError)
    }
}

fn read_object(path: &std::path::Path) -> Result<Map<String, Value>, StoreError> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Map::new()),
        Err(_) => return Err(StoreError),
    };
    let mut bytes = Vec::new();
    file.take(MAX_FILE_SIZE + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| StoreError)?;
    if bytes.len() as u64 > MAX_FILE_SIZE {
        return Err(StoreError);
    }
    parse_object(&bytes)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
/// Sanitized storage failure; deliberately carries no underlying error text.
pub struct StoreError;

impl std::fmt::Display for StoreError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("credential storage operation failed")
    }
}

impl std::error::Error for StoreError {}

struct UniqueValue(Value);

impl<'de> Deserialize<'de> for UniqueValue {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct UniqueVisitor;

        impl<'de> Visitor<'de> for UniqueVisitor {
            type Value = UniqueValue;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("JSON without duplicate object keys")
            }

            fn visit_unit<E: serde::de::Error>(self) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::Null))
            }

            fn visit_bool<E: serde::de::Error>(self, value: bool) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::Bool(value)))
            }

            fn visit_i64<E: serde::de::Error>(self, value: i64) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::Number(value.into())))
            }

            fn visit_u64<E: serde::de::Error>(self, value: u64) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::Number(value.into())))
            }

            fn visit_f64<E: serde::de::Error>(self, value: f64) -> Result<Self::Value, E> {
                Number::from_f64(value)
                    .map(|number| UniqueValue(Value::Number(number)))
                    .ok_or_else(|| E::custom("nonfinite number"))
            }

            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::String(value.to_owned())))
            }

            fn visit_string<E: serde::de::Error>(self, value: String) -> Result<Self::Value, E> {
                Ok(UniqueValue(Value::String(value)))
            }

            fn visit_seq<A: SeqAccess<'de>>(
                self,
                mut sequence: A,
            ) -> Result<Self::Value, A::Error> {
                let mut values = Vec::new();
                while let Some(value) = sequence.next_element::<UniqueValue>()? {
                    values.push(value.0);
                }
                Ok(UniqueValue(Value::Array(values)))
            }

            fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
                let mut values = Map::new();
                while let Some((key, value)) = map.next_entry::<String, UniqueValue>()? {
                    if values.insert(key.clone(), value.0).is_some() {
                        return Err(serde::de::Error::custom(format!("duplicate key: {key}")));
                    }
                }
                Ok(UniqueValue(Value::Object(values)))
            }
        }

        deserializer.deserialize_any(UniqueVisitor)
    }
}

fn parse_object(bytes: &[u8]) -> Result<Map<String, Value>, StoreError> {
    let uncommented = strip_json_comments(bytes)?;
    let value: UniqueValue = serde_json::from_slice(&uncommented).map_err(|_| StoreError)?;
    value.0.as_object().cloned().ok_or(StoreError)
}

/// Removes C++-compatible `//` and `/* */` comments outside JSON strings while
/// preserving quoted text and line boundaries.
fn strip_json_comments(bytes: &[u8]) -> Result<Vec<u8>, StoreError> {
    let mut output = bytes.to_vec();
    let (mut index, mut quoted, mut escaped) = (0, false, false);
    while index < bytes.len() {
        if quoted {
            match bytes[index] {
                b'\\' if !escaped => escaped = true,
                b'"' if !escaped => quoted = false,
                _ => escaped = false,
            }
            index += 1;
            continue;
        }
        if bytes[index] == b'"' {
            quoted = true;
            index += 1;
            continue;
        }
        if bytes[index] == b'/' && index + 1 < bytes.len() {
            let end = match bytes[index + 1] {
                b'/' => {
                    let mut end = index + 2;
                    while end < bytes.len() && bytes[end] != b'\n' && bytes[end] != b'\r' {
                        end += 1;
                    }
                    end
                }
                b'*' => {
                    let mut end = index + 2;
                    while end + 1 < bytes.len() && &bytes[end..end + 2] != b"*/" {
                        end += 1;
                    }
                    if end + 1 >= bytes.len() {
                        return Err(StoreError);
                    }
                    end + 2
                }
                _ => {
                    index += 1;
                    continue;
                }
            };
            for byte in &mut output[index..end] {
                if *byte != b'\n' && *byte != b'\r' {
                    *byte = b' ';
                }
            }
            index = end;
            continue;
        }
        index += 1;
    }
    if quoted {
        return Err(StoreError);
    }
    Ok(output)
}

fn validate_credential(value: &str) -> Result<(), StoreError> {
    if value.len() > 4096 || value.bytes().any(|b| b.is_ascii_control()) {
        return Err(StoreError);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn store(dir: &tempfile::TempDir) -> CredentialStore {
        CredentialStore::new(dir.path(), "speech")
    }

    #[test]
    fn absent_read_does_not_create_file() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        store.read().unwrap();
        assert!(!store.path().exists());
    }

    #[test]
    fn stores_at_expected_path_with_private_mode_and_preserves_fields() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        store.set("api_key", "old").unwrap();
        store.set("api_key", "new").unwrap();
        let mut data: Value = serde_json::from_slice(&fs::read(store.path()).unwrap()).unwrap();
        assert_eq!(store.path(), dir.path().join("plugin/speech/config.json"));
        assert_eq!(
            fs::metadata(store.path()).unwrap().permissions().mode() & 0o777,
            0o600
        );
        data.as_object_mut()
            .unwrap()
            .insert("unknown".into(), Value::Bool(true));
        fs::write(store.path(), serde_json::to_vec(&data).unwrap()).unwrap();
        fs::set_permissions(store.path(), fs::Permissions::from_mode(0o644)).unwrap();
        store.set("api_key", "replacement").unwrap();
        assert_eq!(
            fs::metadata(store.path()).unwrap().permissions().mode() & 0o777,
            0o600
        );
        let data: Value = serde_json::from_slice(&fs::read(store.path()).unwrap()).unwrap();
        assert_eq!(data["api_key"], "replacement");
        assert_eq!(data["unknown"], true);
        store.set("api_key", "").unwrap();
        let data: Value = serde_json::from_slice(&fs::read(store.path()).unwrap()).unwrap();
        assert!(data.get("api_key").is_none());
        assert_eq!(data["unknown"], true);
    }

    #[test]
    fn a_symlinked_credential_file_keeps_its_link_and_updates_the_target() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        let parent = store.path().parent().unwrap();
        fs::create_dir_all(parent).unwrap();
        let target = dir.path().join("real.json");
        fs::write(&target, br#"{"keep":true}"#).unwrap();
        std::os::unix::fs::symlink(&target, store.path()).unwrap();

        store.set("api_key", "secret").unwrap();
        assert!(
            fs::symlink_metadata(store.path())
                .unwrap()
                .file_type()
                .is_symlink(),
            "the link itself must not be replaced by a regular file"
        );
        let data: Value = serde_json::from_slice(&fs::read(&target).unwrap()).unwrap();
        assert_eq!(data["api_key"], "secret");
        assert_eq!(data["keep"], true);
        assert_eq!(
            fs::metadata(&target).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }

    #[test]
    fn a_dangling_credential_symlink_is_rejected_instead_of_replaced() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        std::os::unix::fs::symlink(dir.path().join("missing.json"), store.path()).unwrap();
        assert!(store.set("api_key", "secret").is_err());
        assert!(store.read().is_err());
        assert!(fs::symlink_metadata(store.path())
            .unwrap()
            .file_type()
            .is_symlink());
        assert!(!dir.path().join("missing.json").exists());
    }

    #[test]
    fn credential_read_and_update_accept_cpp_style_comments() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        fs::write(
            store.path(),
            br#"{
                // API credentials are managed by the user.
                "keep": "https://example.test/a//b", /* inline comment */
                "api_key": "old"
            }"#,
        )
        .unwrap();
        store.read().unwrap();
        store.set("api_key", "new").unwrap();
        let object = read_object(store.path()).unwrap();
        assert_eq!(object["keep"], "https://example.test/a//b");
        assert_eq!(object["api_key"], "new");
    }

    #[test]
    fn credential_comments_must_be_terminated_and_duplicate_keys_stay_rejected() {
        assert!(parse_object(br#"{"x":1, /* unterminated }"#).is_err());
        assert!(parse_object(
            br#"{"x":1, // comment
"x":2}"#
        )
        .is_err());
    }

    #[test]
    fn rejects_duplicate_keys_at_any_depth() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        for json in [
            br#"{"secret":"first","secret":"second"}"#.as_slice(),
            br#"{"unknown":{"nested":1,"nested":2}}"#.as_slice(),
        ] {
            fs::write(store.path(), json).unwrap();
            assert!(store.read().is_err());
        }
    }

    #[test]
    fn rejects_invalid_values_and_malformed_input_without_leaking_data() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        for value in ["\0", "\u{7f}", &"x".repeat(4097)] {
            let error = store.set("secret", value).unwrap_err();
            assert!(!error.to_string().contains(value));
        }
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        let malformed = b"{\"secret\":\"topsecret\"";
        fs::write(store.path(), malformed).unwrap();
        let error = store.set("secret", "safe").unwrap_err();
        assert!(!error.to_string().contains("topsecret"));
        assert!(store.read().is_err());
        assert_eq!(fs::read(store.path()).unwrap(), malformed);
    }

    #[test]
    fn unrelated_temporary_file_is_preserved_on_success_and_failure() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        let parent = store.path().parent().unwrap();
        fs::create_dir_all(parent).unwrap();
        let unrelated = parent.join(format!(".config-{}-tmp", std::process::id()));
        fs::write(&unrelated, b"someone else's file").unwrap();
        store.set("secret", "safe").unwrap();
        assert_eq!(fs::read(&unrelated).unwrap(), b"someone else's file");
        assert!(store.set("secret", "\0").is_err());
        fs::write(store.path(), b"malformed").unwrap();
        assert!(store.set("secret", "safe").is_err());
        assert_eq!(fs::read(store.path()).unwrap(), b"malformed");
        assert_eq!(fs::read(&unrelated).unwrap(), b"someone else's file");
        assert_eq!(fs::read_dir(parent).unwrap().count(), 2);
    }

    #[test]
    fn rejects_oversized_files_on_read_and_set_without_modification() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        let mut data = b"{}".to_vec();
        data.resize(MAX_FILE_SIZE as usize + 1, b' ');
        fs::write(store.path(), &data).unwrap();
        assert!(store.read().is_err());
        assert!(store.set("secret", "safe").is_err());
        assert_eq!(fs::read(store.path()).unwrap(), data);
    }

    #[test]
    fn accepts_size_limit_but_rejects_updates_exceeding_it() {
        let dir = tempfile::tempdir().unwrap();
        let store = store(&dir);
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        let data = serde_json::to_vec(&serde_json::json!({
            "padding": "x".repeat(MAX_FILE_SIZE as usize - 14)
        }))
        .unwrap();
        assert_eq!(data.len() as u64, MAX_FILE_SIZE);
        fs::write(store.path(), &data).unwrap();
        store.read().unwrap();
        assert!(store.set("secret", "safe").is_err());
        assert_eq!(fs::read(store.path()).unwrap(), data);
        store.set("padding", "").unwrap();
        store.read().unwrap();
        assert_eq!(fs::read(store.path()).unwrap(), b"{}");
    }
}
