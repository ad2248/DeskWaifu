use reqwest::{Url, header::HeaderValue};
use serde::Deserialize;
use std::{
    env,
    fs::OpenOptions,
    io::Read,
    os::unix::fs::{OpenOptionsExt, PermissionsExt},
    path::PathBuf,
};

pub const DEFAULT_ENDPOINT: &str =
    "wss://llm-y3exskfcgxgxzn23.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference";
pub const DEFAULT_MODEL: &str = "qwen-audio-3.1-asr-flash-streaming";
pub const DEFAULT_SECONDS: u64 = 60;
const CONFIG_LIMIT: u64 = 16 * 1024;

pub struct Config {
    pub endpoint: Url,
    pub model: String,
    pub max_recording_seconds: u64,
    pub authorization: Option<HeaderValue>,
}

#[derive(Deserialize)]
#[serde(default, deny_unknown_fields)]
struct FileConfig {
    endpoint: String,
    model: String,
    max_recording_seconds: u64,
    api_key: String,
}

impl Default for FileConfig {
    fn default() -> Self {
        Self {
            endpoint: DEFAULT_ENDPOINT.into(),
            model: DEFAULT_MODEL.into(),
            max_recording_seconds: DEFAULT_SECONDS,
            api_key: String::new(),
        }
    }
}

pub fn path() -> Option<PathBuf> {
    env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .or_else(|| env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .map(|path| path.join("deskwaifu/voice_input.json"))
}

impl Config {
    pub fn load(path: Option<PathBuf>) -> Result<Self, &'static str> {
        let path = path.ok_or("config_unavailable")?;
        let file = match OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NONBLOCK)
            .open(path)
        {
            Ok(file) => file,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                return FileConfig::default().try_into();
            }
            Err(_) => return Err("config_unavailable"),
        };
        let metadata = file.metadata().map_err(|_| "config_unavailable")?;
        if !metadata.is_file() {
            return Err("invalid_config");
        }
        if metadata.permissions().mode() & 0o777 != 0o600 {
            return Err("config_permissions");
        }
        let mut bytes = Vec::new();
        file.take(CONFIG_LIMIT + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| "config_unavailable")?;
        if bytes.len() as u64 > CONFIG_LIMIT {
            return Err("invalid_config");
        }
        serde_json::from_slice::<FileConfig>(&bytes)
            .map_err(|_| "invalid_config")?
            .try_into()
    }
}

impl TryFrom<FileConfig> for Config {
    type Error = &'static str;

    fn try_from(config: FileConfig) -> Result<Self, Self::Error> {
        let endpoint = Url::parse(&config.endpoint).map_err(|_| "invalid_config")?;
        if !matches!(endpoint.scheme(), "https" | "wss")
            || !endpoint.has_host()
            || !endpoint.username().is_empty()
            || endpoint.password().is_some()
            || endpoint.fragment().is_some()
            || config.endpoint.len() > 4096
            || config.model.trim().is_empty()
            || config.model.len() > 256
            || config.model.chars().any(char::is_control)
            || !(1..=300).contains(&config.max_recording_seconds)
            || config.api_key.len() > 4096
            || config.api_key.chars().any(char::is_whitespace)
            || config.api_key.chars().any(char::is_control)
        {
            return Err("invalid_config");
        }
        let authorization = if config.api_key.is_empty() {
            None
        } else {
            let mut value = HeaderValue::from_str(&format!("Bearer {}", config.api_key))
                .map_err(|_| "invalid_config")?;
            value.set_sensitive(true);
            Some(value)
        };
        Ok(Self {
            endpoint,
            model: config.model,
            max_recording_seconds: config.max_recording_seconds,
            authorization,
        })
    }
}
