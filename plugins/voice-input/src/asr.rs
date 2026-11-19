use crate::config::Config;
use base64::{Engine, engine::general_purpose::STANDARD};
use futures_util::{SinkExt, StreamExt};
use reqwest::{Url, header::AUTHORIZATION};
use serde_json::{Value, json};
use std::time::Duration;
use tokio::time::{Instant, sleep_until, timeout};
use tokio_tungstenite::{
    connect_async_with_config,
    tungstenite::{self, Message, client::IntoClientRequest, protocol::WebSocketConfig},
};
use uuid::Uuid;

pub const MAX_TEXT: usize = 1024 * 1024;
pub const PCM_PER_SECOND: usize = 32_000;
const START_TIMEOUT: Duration = Duration::from_secs(15);
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(120);
const PACKET_INTERVAL: Duration = Duration::from_millis(100);
const PACKET_BYTES: usize = PCM_PER_SECOND / 10;
const TAIL_BYTES: usize = PCM_PER_SECOND / 4;
const NATIVE_PATH: &str = "/api/v1/services/aigc/multimodal-generation/generation";

type Result<T> = std::result::Result<T, &'static str>;

pub async fn recognize(config: Config, pcm: Vec<u8>) -> Result<String> {
    if pcm.is_empty() {
        return Err("no_audio");
    }
    if pcm.len() % 2 != 0 {
        return Err("invalid_audio");
    }
    if config.endpoint.scheme() == "wss" {
        let deadline =
            RESPONSE_TIMEOUT + Duration::from_millis((pcm.len() + TAIL_BYTES) as u64 / 32);
        timeout(deadline, streaming(config, pcm, START_TIMEOUT))
            .await
            .map_err(|_| "asr_timeout")?
    } else {
        let (url, compatible) = http_endpoint(config.endpoint.clone(), &config.model);
        timeout(RESPONSE_TIMEOUT, https(config, url, compatible, &pcm))
            .await
            .map_err(|_| "asr_timeout")?
    }
}

fn ws_error(error: tungstenite::Error) -> &'static str {
    match error {
        tungstenite::Error::Capacity(_) => "asr_response_too_large",
        tungstenite::Error::Protocol(_)
        | tungstenite::Error::Utf8(_)
        | tungstenite::Error::ConnectionClosed => "asr_protocol",
        _ => "asr_network",
    }
}

fn event(message: Message, id: &str) -> Result<Option<Value>> {
    match message {
        Message::Ping(_) | Message::Pong(_) => Ok(None),
        Message::Text(text) => {
            let value: Value = serde_json::from_str(&text).map_err(|_| "asr_protocol")?;
            if value["header"]["task_id"].as_str() != Some(id) {
                return Err("asr_protocol");
            }
            if value["header"]["event"] == "task-failed" {
                return Err("asr_task_failed");
            }
            Ok(Some(value))
        }
        _ => Err("asr_protocol"),
    }
}

fn checked_text(text: String) -> Result<String> {
    if text.len() > MAX_TEXT {
        Err("asr_response_too_large")
    } else if text.trim().is_empty() {
        Err("no_speech")
    } else {
        Ok(text)
    }
}

async fn streaming(config: Config, mut pcm: Vec<u8>, start_timeout: Duration) -> Result<String> {
    let mut request = config
        .endpoint
        .as_str()
        .into_client_request()
        .map_err(|_| "invalid_config")?;
    request
        .headers_mut()
        .insert(AUTHORIZATION, config.authorization.ok_or("not_configured")?);
    let id = Uuid::new_v4().to_string();
    let (mut socket, ()) = timeout(start_timeout, async {
        let (mut socket, _) = connect_async_with_config(
            request,
            Some(WebSocketConfig::default().max_message_size(Some(MAX_TEXT)).max_frame_size(Some(MAX_TEXT))),
            true,
        ).await.map_err(ws_error)?;
        socket.send(Message::Text(json!({
            "header": {"action": "run-task", "task_id": id, "streaming": "duplex"},
            "payload": {
                "task_group": "audio", "task": "asr", "function": "recognition", "model": config.model,
                "parameters": {"format": "pcm", "sample_rate": 16000}, "input": {}
            }
        }).to_string().into())).await.map_err(ws_error)?;
        loop {
            let message = socket.next().await.ok_or("asr_protocol")?.map_err(ws_error)?;
            if let Some(value) = event(message, &id)? {
                if value["header"]["event"] == "task-started" {
                    return Ok((socket, ()));
                }
                return Err("asr_protocol");
            }
        }
    }).await.map_err(|_| "asr_timeout")??;

    pcm.resize(pcm.len() + TAIL_BYTES, 0);
    let mut offset = 0;
    let mut finish_sent = false;
    let mut result = String::new();
    let mut next_packet = Instant::now() + PACKET_INTERVAL;
    loop {
        tokio::select! {
            message = socket.next() => {
                let message = message.ok_or("asr_protocol")?.map_err(ws_error)?;
                let Some(value) = event(message, &id)? else { continue };
                match value["header"]["event"].as_str() {
                    Some("result-generated") => {
                        let sentence = &value["payload"]["output"]["sentence"];
                        if sentence["heartbeat"] == true {
                            continue;
                        }
                        let text = sentence["text"].as_str().ok_or("asr_protocol")?;
                        if sentence["sentence_end"].as_bool().ok_or("asr_protocol")? {
                            if result.len() + text.len() > MAX_TEXT {
                                return Err("asr_response_too_large");
                            }
                            result.push_str(text);
                        }
                    }
                    Some("task-finished") if finish_sent => return checked_text(result),
                    _ => return Err("asr_protocol"),
                }
            }
            _ = sleep_until(next_packet), if !finish_sent => {
                let message = if offset < pcm.len() {
                    let end = (offset + PACKET_BYTES).min(pcm.len());
                    let message = Message::Binary(pcm[offset..end].to_vec().into());
                    offset = end;
                    message
                } else {
                    finish_sent = true;
                    Message::Text(json!({
                        "header": {"action": "finish-task", "task_id": id, "streaming": "duplex"},
                        "payload": {"input": {}}
                    }).to_string().into())
                };
                socket.send(message).await.map_err(ws_error)?;
                next_packet = Instant::now() + PACKET_INTERVAL;
            }
        }
    }
}

fn http_endpoint(mut url: Url, model: &str) -> (Url, bool) {
    let path = url.path().trim_end_matches('/');
    let base = path.ends_with("/v1");
    let compatible = base || path.ends_with("/chat/completions");
    if compatible && model == "qwen-audio-3.0-asr-flash" {
        url.set_path(NATIVE_PATH);
        return (url, false);
    }
    if base {
        let path = format!("{path}/chat/completions");
        url.set_path(&path);
    }
    (url, compatible)
}

fn wav(pcm: &[u8]) -> Vec<u8> {
    let size = pcm.len() as u32;
    let mut wav = Vec::with_capacity(pcm.len() + 44);
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(size + 36).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16u32.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes()); // PCM
    wav.extend_from_slice(&1u16.to_le_bytes()); // 单声道
    wav.extend_from_slice(&16_000u32.to_le_bytes());
    wav.extend_from_slice(&(PCM_PER_SECOND as u32).to_le_bytes());
    wav.extend_from_slice(&2u16.to_le_bytes()); // 每帧字节数
    wav.extend_from_slice(&16u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&size.to_le_bytes());
    wav.extend_from_slice(pcm);
    wav
}

async fn https(config: Config, url: Url, compatible: bool, pcm: &[u8]) -> Result<String> {
    let client = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .retry(reqwest::retry::never())
        .connect_timeout(START_TIMEOUT)
        .build()
        .map_err(|_| "asr_network")?;
    let data = format!("data:audio/wav;base64,{}", STANDARD.encode(wav(pcm)));
    let messages = json!([{"role": "user", "content": [{"type": "input_audio", "input_audio": {"data": data}}]}]);
    let (mut request, body) = if compatible {
        (
            client.post(url),
            json!({"model": config.model, "messages": messages, "stream": false}),
        )
    } else {
        (
            client.post(url).header("X-DashScope-SSE", "disable"),
            json!({
                "model": config.model, "input": {"messages": messages},
                "parameters": {"format": "wav", "sample_rate": "16000"}
            }),
        )
    };
    request = request.header(AUTHORIZATION, config.authorization.ok_or("not_configured")?);
    let mut response = request.json(&body).send().await.map_err(|error| {
        if error.is_timeout() {
            "asr_timeout"
        } else {
            "asr_network"
        }
    })?;
    if !response.status().is_success() {
        return Err("asr_http");
    }
    if response
        .content_length()
        .is_some_and(|len| len > MAX_TEXT as u64)
    {
        return Err("asr_response_too_large");
    }
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|_| "asr_network")? {
        if body.len() + chunk.len() > MAX_TEXT {
            return Err("asr_response_too_large");
        }
        body.extend_from_slice(&chunk);
    }
    let value: Value = serde_json::from_slice(&body).map_err(|_| "asr_protocol")?;
    let text = if compatible {
        value["choices"][0]["message"]["content"].as_str()
    } else {
        value["text"]
            .as_str()
            .or_else(|| value["output"]["text"].as_str())
    }
    .ok_or("asr_protocol")?;
    checked_text(text.to_owned())
}

#[cfg(test)]
mod tests;
