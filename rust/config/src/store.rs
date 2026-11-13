//! Filesystem-backed configuration storage with snapshot conflict detection.
//!
//! This mirrors the accepted and rejected filesystem cases of the C++ `ConfigStore`
//! contract in `sdk/contracts/ConfigStore.cpp`:
//!
//! - a missing file reads as an empty document;
//! - non-regular and oversized files are rejected on read;
//! - a commit refuses to replace a document whose top-level values no longer match
//!   the caller's snapshot, unless `overwrite` skips that comparison;
//! - saves replace the whole document atomically through a same-directory temporary
//!   file and `rename`, keeping the existing file mode unless private mode forces
//!   `0600`;
//! - a commit re-reads the file before `rename`, so a change made after the snapshot
//!   comparison is not silently lost.
//!
//! JSON syntax and value canonicalization come from [`ConfigDocument`]. Business
//! validation stays with the caller, except for [`Store::commit_settings`], which
//! applies [`Settings::decode`] the way the desktop settings path does before it
//! saves. The commit-time re-read only narrows the race window; it is not a
//! compare-and-swap guarantee against arbitrary external writers, matching the C++
//! contract.

use crate::settings::{ConfigDocument, Settings};
use std::fmt;
use std::fs::{self, File};
use std::io::{ErrorKind, Read, Write};
use std::os::unix::fs::{chown, DirBuilderExt, MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};

const MAX_FILE_SIZE: u64 = 4 * 1024 * 1024;
const EMPTY_DOCUMENT: &str = "{}\n";
const INVALID_DOCUMENT: &str = "Invalid JSON configuration or duplicate key";
const NOT_REGULAR: &str = "Configuration must be a regular file";
const OVERSIZED: &str = "Config exceeds 4 MiB";
const UNRESOLVED_PATH: &str = "Cannot resolve config path (possibly a dangling symbolic link)";
const CONFLICT: &str = "Config conflict: file changed";
const CHANGED_DURING_SAVE: &str = "Config changed during save; retry";

/// Sanitized storage failure; the message never includes document content.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StoreError {
    message: String,
}

impl StoreError {
    fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for StoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for StoreError {}

impl From<std::io::Error> for StoreError {
    fn from(error: std::io::Error) -> Self {
        Self::new(error.to_string())
    }
}

/// Stores `<config_root>/deskwaifu/config.json`.
#[derive(Clone, Debug)]
pub struct Store {
    path: PathBuf,
    private_permissions: bool,
}

impl Store {
    /// Construct a store that keeps an existing file's permissions on every save.
    pub fn new(config_root: impl Into<PathBuf>) -> Self {
        Self::with_private_permissions(config_root, false)
    }

    /// Construct a store that forces mode `0600` on every save.
    pub fn new_private(config_root: impl Into<PathBuf>) -> Self {
        Self::with_private_permissions(config_root, true)
    }

    fn with_private_permissions(
        config_root: impl Into<PathBuf>,
        private_permissions: bool,
    ) -> Self {
        Self {
            path: config_root.into().join("deskwaifu").join("config.json"),
            private_permissions,
        }
    }

    /// Return the backing file path.
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Read and parse the document, treating a missing file as an empty object.
    ///
    /// A parse or read failure returns an error and leaves any caller snapshot
    /// untouched.
    pub fn read(&self) -> Result<ConfigDocument, StoreError> {
        let path = resolve(&self.path)?;
        let text = read_text(&path)?;
        ConfigDocument::parse(&text, true).map_err(|_| StoreError::new(INVALID_DOCUMENT))
    }

    /// Validate and atomically replace the whole document.
    ///
    /// The current top-level values are compared with `expected` unless `overwrite`
    /// is set; `overwrite` does not skip the re-read that happens at save time. On
    /// success the saved snapshot is returned; on failure the file and the caller's
    /// snapshot are unchanged.
    pub fn commit(
        &self,
        document: &str,
        expected: &ConfigDocument,
        overwrite: bool,
    ) -> Result<ConfigDocument, StoreError> {
        if document.len() as u64 > MAX_FILE_SIZE {
            return Err(StoreError::new(OVERSIZED));
        }
        let path = resolve(&self.path)?;
        let previous = read_text(&path)?;
        let latest = ConfigDocument::parse(&previous, true)
            .map_err(|_| StoreError::new(INVALID_DOCUMENT))?;
        if !overwrite && latest.values != expected.values {
            return Err(StoreError::new(CONFLICT));
        }
        let next =
            ConfigDocument::parse(document, true).map_err(|_| StoreError::new(INVALID_DOCUMENT))?;
        self.save(&path, document, &previous, next)
    }

    /// Like [`Store::commit`], but first decode the document as desktop settings.
    ///
    /// This matches the settings path, where the document is serialized, parsed and
    /// decoded before `ConfigStore::Commit` sees it. `ConfigStore::Commit` itself
    /// validates JSON syntax only.
    pub fn commit_settings(
        &self,
        document: &str,
        expected: &ConfigDocument,
        overwrite: bool,
    ) -> Result<ConfigDocument, StoreError> {
        let parsed =
            ConfigDocument::parse(document, true).map_err(|_| StoreError::new(INVALID_DOCUMENT))?;
        Settings::decode(&parsed, &mut Settings::default()).map_err(StoreError::new)?;
        self.commit(document, expected, overwrite)
    }

    fn save(
        &self,
        path: &Path,
        document: &str,
        previous: &str,
        next: ConfigDocument,
    ) -> Result<ConfigDocument, StoreError> {
        let directory = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(directory)?;
        let original = fs::metadata(path).ok();
        let mut temporary = tempfile::Builder::new()
            .prefix(".deskwaifu-config-")
            .tempfile_in(directory)?;
        let mode = match (&original, self.private_permissions) {
            (_, true) | (None, false) => 0o600,
            (Some(info), false) => info.permissions().mode() & 0o777,
        };
        temporary
            .as_file()
            .set_permissions(fs::Permissions::from_mode(mode))?;
        if let Some(info) = &original {
            let created = temporary.as_file().metadata()?;
            if created.uid() != info.uid() || created.gid() != info.gid() {
                chown(temporary.path(), Some(info.uid()), Some(info.gid()))?;
            }
        }
        temporary.write_all(document.as_bytes())?;
        temporary.as_file().sync_all()?;
        // The C++ contract reports any re-read problem as a concurrent change.
        if read_text(path).ok().as_deref() != Some(previous) {
            return Err(StoreError::new(CHANGED_DURING_SAVE));
        }
        temporary.persist(path).map_err(|error| error.error)?;
        if let Ok(directory) = File::open(directory) {
            let _ = directory.sync_all();
        }
        Ok(next)
    }
}

/// Resolve the config path like `realpath`, keeping a path whose leaf does not exist.
pub(crate) fn resolve(path: &Path) -> Result<PathBuf, StoreError> {
    match fs::canonicalize(path) {
        Ok(resolved) => Ok(resolved),
        Err(error) => {
            if fs::symlink_metadata(path).is_ok() || error.kind() != ErrorKind::NotFound {
                return Err(StoreError::new(UNRESOLVED_PATH));
            }
            Ok(path.to_owned())
        }
    }
}

/// Read the document text, treating only a missing leaf as an empty object.
fn read_text(path: &Path) -> Result<String, StoreError> {
    match fs::metadata(path) {
        Ok(info) if !info.is_file() => return Err(StoreError::new(NOT_REGULAR)),
        Ok(_) => {}
        Err(error) if error.kind() == ErrorKind::NotFound => return Ok(EMPTY_DOCUMENT.into()),
        Err(error) => return Err(error.into()),
    }
    let file = File::open(path)?;
    if !file.metadata()?.is_file() {
        return Err(StoreError::new(NOT_REGULAR));
    }
    let mut bytes = Vec::new();
    file.take(MAX_FILE_SIZE + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_FILE_SIZE {
        return Err(StoreError::new(OVERSIZED));
    }
    String::from_utf8(bytes).map_err(|_| StoreError::new(INVALID_DOCUMENT))
}
