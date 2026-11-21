use reqwest::{Url, header::HeaderValue};
use serde::Deserialize;
pub const DEFAULT_ENDPOINT: &str =
    "wss://llm-y3exskfcgxgxzn23.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference";
pub const DEFAULT_MODEL: &str = "qwen-audio-3.1-asr-flash-streaming";
pub const DEFAULT_SECONDS: u64 = 60;

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

impl Config {
    pub fn from_section(section: Option<serde_json::Value>) -> Result<Self, &'static str> {
        let file = match section {
            Some(value) if value.is_object() => {
                serde_json::from_value(value).map_err(|_| "invalid_config")?
            }
            Some(_) => return Err("invalid_config"),
            None => FileConfig::default(),
        };
        file.try_into()
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
