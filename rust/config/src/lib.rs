//! Private credential-file storage.
//!
//! This slice does not provide sound concurrency-conflict protection: concurrent
//! writers can overwrite one another. Callers must not treat it as the complete
//! ConfigStore semantics described by the application contract.

use serde::de::{MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};
use serde_json::{Map, Number, Value};
use std::fmt;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::PathBuf;

const MAX_FILE_SIZE: u64 = 4 * 1024 * 1024;

/// Stores a named credential in `<root>/plugin/<plugin>/config.json`.
pub struct CredentialStore {
    path: PathBuf,
}

impl CredentialStore {
    /// Construct a store using an explicit config root and plugin name.
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
        let bytes = match fs::read(&self.path) {
            Ok(bytes) => bytes,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(_) => return Err(StoreError),
        };
        if bytes.len() as u64 > MAX_FILE_SIZE {
            return Err(StoreError);
        }
        parse_object(&bytes).map(|_| ())
    }

    /// Set or clear a credential. Empty values remove the named field.
    pub fn set(&self, name: &str, credential: &str) -> Result<(), StoreError> {
        validate_credential(credential)?;
        let mut object = match fs::read(&self.path) {
            Ok(bytes) => parse_object(&bytes)?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Map::new(),
            Err(_) => return Err(StoreError),
        };
        if credential.is_empty() {
            object.remove(name);
        } else {
            object.insert(name.to_owned(), Value::String(credential.to_owned()));
        }
        let data = serde_json::to_vec(&Value::Object(object)).map_err(|_| StoreError)?;
        let parent = self.path.parent().ok_or(StoreError)?;
        fs::create_dir_all(parent).map_err(|_| StoreError)?;
        let temp = parent.join(format!(".config-{}-tmp", std::process::id()));
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(&temp)
                .map_err(|_| StoreError)?;
            file.write_all(&data).map_err(|_| StoreError)?;
            file.sync_all().map_err(|_| StoreError)?;
            fs::set_permissions(&temp, fs::Permissions::from_mode(0o600))
                .map_err(|_| StoreError)?;
            fs::rename(&temp, &self.path).map_err(|_| StoreError)?;
            Ok(())
        })();
        if result.is_err() {
            let _ = fs::remove_file(temp);
        }
        result
    }
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
    let value: UniqueValue = serde_json::from_slice(bytes).map_err(|_| StoreError)?;
    value.0.as_object().cloned().ok_or(StoreError)
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
        store.set("api_key", "replacement").unwrap();
        let data: Value = serde_json::from_slice(&fs::read(store.path()).unwrap()).unwrap();
        assert_eq!(data["api_key"], "replacement");
        assert_eq!(data["unknown"], true);
        store.set("api_key", "").unwrap();
        let data: Value = serde_json::from_slice(&fs::read(store.path()).unwrap()).unwrap();
        assert!(data.get("api_key").is_none());
        assert_eq!(data["unknown"], true);
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
        fs::write(store.path(), b"{\"secret\":\"topsecret\"").unwrap();
        let error = store.set("secret", "safe").unwrap_err();
        assert!(!error.to_string().contains("topsecret"));
        assert!(store.read().is_err());
    }
}
