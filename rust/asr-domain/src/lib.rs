//! Bailian/DashScope duplex streaming speech recognition.
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::time::Duration;
use tokio_tungstenite::{
    connect_async_with_config,
    tungstenite::{
        client::IntoClientRequest, http::header::AUTHORIZATION, protocol::WebSocketConfig, Message,
    },
};

const MAX_MESSAGE: usize = 1024 * 1024;
const CHUNK_SIZE: usize = 3200;
const CHUNK_INTERVAL: Duration = Duration::from_millis(100);

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AsrError {
    InvalidEndpoint,
    InvalidApiKey,
    NoAudio,
    InvalidAudio,
    Network,
    Timeout,
    Protocol,
    Remote,
    NoSpeech,
    ResponseTooLarge,
}

pub async fn recognize_stream(
    endpoint: impl AsRef<str>,
    api_key: impl AsRef<str>,
    model: impl AsRef<str>,
    pcm16le: &[u8],
    timeout: Duration,
) -> Result<String, AsrError> {
    let endpoint = endpoint.as_ref();
    let key = api_key.as_ref();
    let model = model.as_ref();
    if key.is_empty() || key.len() > 4096 || key.chars().any(char::is_control) {
        return Err(AsrError::InvalidApiKey);
    }
    if model.is_empty() || model.chars().any(char::is_control) {
        return Err(AsrError::Protocol);
    }
    if pcm16le.is_empty() {
        return Err(AsrError::NoAudio);
    }
    if !pcm16le.len().is_multiple_of(2) {
        return Err(AsrError::InvalidAudio);
    }
    let parsed = url::Url::parse(endpoint).map_err(|_| AsrError::InvalidEndpoint)?;
    let local_ws = parsed.scheme() == "ws"
        && matches!(parsed.host_str(), Some("localhost" | "127.0.0.1" | "::1"));
    if (parsed.scheme() != "wss" && !local_ws)
        || parsed.host_str().is_none()
        || !parsed.username().is_empty()
        || parsed.password().is_some()
        || parsed.fragment().is_some()
    {
        return Err(AsrError::InvalidEndpoint);
    }
    let mut request = endpoint
        .into_client_request()
        .map_err(|_| AsrError::InvalidEndpoint)?;
    request.headers_mut().insert(
        AUTHORIZATION,
        format!("Bearer {key}")
            .parse()
            .map_err(|_| AsrError::InvalidApiKey)?,
    );
    let config = WebSocketConfig {
        max_message_size: Some(MAX_MESSAGE),
        max_frame_size: Some(MAX_MESSAGE),
        ..Default::default()
    };
    let total_timeout =
        Duration::from_millis(120_000u64.saturating_add((pcm16le.len() / 32) as u64));
    let deadline = timeout.min(total_timeout);
    tokio::time::timeout(deadline, run(request, config, model, pcm16le))
        .await
        .map_err(|_| AsrError::Timeout)?
}

async fn run(
    request: tokio_tungstenite::tungstenite::http::Request<()>,
    config: WebSocketConfig,
    model: &str,
    pcm: &[u8],
) -> Result<String, AsrError> {
    let (mut socket, _) = tokio::time::timeout(
        Duration::from_secs(15),
        connect_async_with_config(request, Some(config), false),
    )
    .await
    .map_err(|_| AsrError::Timeout)?
    .map_err(|_| AsrError::Network)?;

    let task_id = uuid::Uuid::new_v4().to_string();
    let run_task = json!({
        "header": {"action":"run-task", "task_id":task_id, "streaming":"duplex"},
        "payload": {
            "task_group":"audio", "task":"asr", "function":"recognition", "model":model,
            "parameters":{"format":"pcm", "sample_rate":16000}, "input":{}
        }
    });
    socket
        .send(Message::Text(run_task.to_string()))
        .await
        .map_err(|_| AsrError::Network)?;

    let mut started = false;
    let mut finish_sent = false;
    let mut offset = 0usize;
    let mut text = String::new();
    let mut send_tick: Option<tokio::time::Interval> = None;
    loop {
        let message = tokio::select! {
            _ = async { if let Some(tick) = send_tick.as_mut() { tick.tick().await } else { std::future::pending().await } } => {
                if offset < pcm.len() {
                    let end = (offset + CHUNK_SIZE).min(pcm.len());
                    socket.send(Message::Binary(pcm[offset..end].to_vec())).await.map_err(|_| AsrError::Network)?;
                    offset = end;
                } else {
                    let finish = json!({"header":{"action":"finish-task","task_id":task_id,"streaming":"duplex"},"payload":{"input":{}}});
                    socket.send(Message::Text(finish.to_string())).await.map_err(|_| AsrError::Network)?;
                    finish_sent = true;
                    send_tick = None;
                }
                continue;
            }
            message = socket.next() => message,
        };
        let message = match message {
            None | Some(Err(_)) => return Err(AsrError::Network),
            Some(Ok(Message::Text(text))) => text,
            Some(Ok(Message::Binary(_))) => return Err(AsrError::Protocol),
            Some(Ok(Message::Close(_))) => return Err(AsrError::Protocol),
            Some(Ok(_)) => continue,
        };
        if message.len() > MAX_MESSAGE {
            return Err(AsrError::ResponseTooLarge);
        }
        let value: Value = serde_json::from_str(&message).map_err(|_| AsrError::Protocol)?;
        let header = value.get("header").ok_or(AsrError::Protocol)?;
        if header.get("task_id").and_then(Value::as_str) != Some(&task_id) {
            return Err(AsrError::Protocol);
        }
        match header
            .get("event")
            .and_then(Value::as_str)
            .ok_or(AsrError::Protocol)?
        {
            "task-failed" => return Err(AsrError::Remote),
            "task-started" if !started => {
                started = true;
                let mut interval = tokio::time::interval(CHUNK_INTERVAL);
                interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
                interval.tick().await;
                send_tick = Some(interval);
            }
            "result-generated" if started => {
                let sentence = value
                    .pointer("/payload/output/sentence")
                    .ok_or(AsrError::Protocol)?;
                if sentence.get("heartbeat").and_then(Value::as_bool) == Some(true) {
                    continue;
                }
                let part = sentence
                    .get("text")
                    .and_then(Value::as_str)
                    .ok_or(AsrError::Protocol)?;
                let final_sentence = sentence
                    .get("sentence_end")
                    .and_then(Value::as_bool)
                    .ok_or(AsrError::Protocol)?;
                if final_sentence {
                    if text.len().saturating_add(part.len()) > MAX_MESSAGE {
                        return Err(AsrError::ResponseTooLarge);
                    }
                    text.push_str(part);
                }
            }
            "task-finished" if finish_sent => {
                return if text.trim().is_empty() {
                    Err(AsrError::NoSpeech)
                } else {
                    Ok(text)
                };
            }
            _ => return Err(AsrError::Protocol),
        }
    }
}
