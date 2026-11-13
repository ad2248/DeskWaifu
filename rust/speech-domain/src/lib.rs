//! MiniMax WebSocket text-to-speech protocol.
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::time::Duration;
use tokio::sync::mpsc as tokio_mpsc;
use tokio_tungstenite::{
    connect_async_with_config,
    tungstenite::{protocol::WebSocketConfig, Message},
};

pub mod player;

const MAX_MESSAGE: usize = 2 * 1024 * 1024;
const MAX_IDLE: Duration = Duration::from_secs(30);

#[derive(Clone, Debug)]
pub struct SpeechRequest {
    pub id: u64,
    pub text: String,
    pub voice_id: String,
    pub model: String,
    pub speed: f64,
    pub volume: f64,
    pub pitch: i32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    Ready {
        sample_rate: u32,
        channels: u16,
        format: PcmFormat,
    },
    Audio(Vec<u8>),
    Completed,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PcmFormat {
    Pcm16,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidRequest,
    MissingApiKey,
    Network,
    Timeout,
    Protocol,
    Authentication,
    Synthesis(i64),
}
pub type Events = tokio_mpsc::Receiver<Result<Event, Error>>;

pub async fn synthesize(
    endpoint: impl Into<String>,
    api_key: impl Into<String>,
    request: SpeechRequest,
    timeout: Duration,
) -> Events {
    let (tx, rx) = tokio_mpsc::channel(8);
    tokio::spawn(run(endpoint.into(), api_key.into(), request, timeout, tx));
    rx
}

fn valid(r: &SpeechRequest) -> bool {
    r.id != 0
        && !r.text.trim().is_empty()
        && r.text.encode_utf16().count() < 10_000
        && !r.voice_id.is_empty()
        && !r.model.is_empty()
        && r.speed.is_finite()
        && (0.5..=2.0).contains(&r.speed)
        && r.volume.is_finite()
        && r.volume > 0.0
        && r.volume <= 10.0
        && (-12..=12).contains(&r.pitch)
}

async fn send_error(tx: &tokio_mpsc::Sender<Result<Event, Error>>, error: Error) {
    let _ = tx.send(Err(error)).await;
}

async fn run(
    endpoint: String,
    key: String,
    req: SpeechRequest,
    timeout: Duration,
    tx: tokio_mpsc::Sender<Result<Event, Error>>,
) {
    if !valid(&req) {
        send_error(&tx, Error::InvalidRequest).await;
        return;
    }
    if key.trim().is_empty() {
        send_error(&tx, Error::MissingApiKey).await;
        return;
    }
    use tokio_tungstenite::tungstenite::{client::IntoClientRequest, http::header::AUTHORIZATION};
    let parsed_endpoint = match url::Url::parse(&endpoint) {
        Ok(url) => url,
        Err(_) => {
            send_error(&tx, Error::InvalidRequest).await;
            return;
        }
    };
    let loopback_ws = parsed_endpoint.scheme() == "ws"
        && matches!(
            parsed_endpoint.host_str(),
            Some("127.0.0.1" | "::1" | "localhost")
        );
    if (parsed_endpoint.scheme() != "wss" && !loopback_ws)
        || parsed_endpoint.username() != ""
        || parsed_endpoint.password().is_some()
        || parsed_endpoint.fragment().is_some()
    {
        send_error(&tx, Error::InvalidRequest).await;
        return;
    }
    let mut request = match endpoint.as_str().into_client_request() {
        Ok(r) => r,
        Err(_) => {
            send_error(&tx, Error::InvalidRequest).await;
            return;
        }
    };
    let auth = match format!("Bearer {key}").parse() {
        Ok(v) => v,
        Err(_) => {
            send_error(&tx, Error::Network).await;
            return;
        }
    };
    request.headers_mut().insert(AUTHORIZATION, auth);
    let config = WebSocketConfig {
        max_message_size: Some(MAX_MESSAGE),
        max_frame_size: Some(MAX_MESSAGE),
        ..Default::default()
    };
    let connection = tokio::select! {
        _ = tx.closed() => return,
        connection = tokio::time::timeout(
            timeout,
            connect_async_with_config(request, Some(config), false),
        ) => connection,
    };
    let (mut socket, _) = match connection {
        Err(_) => {
            send_error(&tx, Error::Timeout).await;
            return;
        }
        Ok(Ok(v)) => v,
        Ok(Err(_)) => {
            send_error(&tx, Error::Network).await;
            return;
        }
    };
    let mut phase = 0u8;
    let idle = MAX_IDLE.min(timeout.max(Duration::from_millis(1)));
    let mut deadline = tokio::time::Instant::now() + idle;
    loop {
        let next = tokio::select! {
            _ = tx.closed() => return,
            _ = tokio::time::sleep_until(deadline) => {
                send_error(&tx, Error::Timeout).await;
                return;
            }
            message = socket.next() => message,
        };
        let msg = match next {
            None | Some(Err(_)) => {
                send_error(&tx, Error::Network).await;
                return;
            }
            Some(Ok(m)) => m,
        };
        let text = match msg {
            Message::Text(t) => t,
            Message::Binary(b) if b.len() <= MAX_MESSAGE => {
                send_error(&tx, Error::Protocol).await;
                return;
            }
            Message::Close(_) => {
                send_error(&tx, Error::Network).await;
                return;
            }
            _ => continue,
        };
        if text.len() > MAX_MESSAGE {
            send_error(&tx, Error::Protocol).await;
            return;
        }
        let v: Value = match serde_json::from_str::<Value>(&text) {
            Ok(v) if v.is_object() => v,
            _ => {
                send_error(&tx, Error::Protocol).await;
                return;
            }
        };
        let Some(code) = v.pointer("/base_resp/status_code").and_then(Value::as_i64) else {
            send_error(&tx, Error::Protocol).await;
            return;
        };
        if code != 0 {
            send_error(
                &tx,
                if code == 1004 {
                    Error::Authentication
                } else {
                    Error::Synthesis(code)
                },
            )
            .await;
            return;
        }
        let event = v.get("event").and_then(Value::as_str).unwrap_or("");
        match (phase, event) {
            (0, "connected_success") => {
                phase = 1;
                let start = json!({"event":"task_start","model":req.model,"voice_setting":{"voice_id":req.voice_id,"speed":req.speed,"vol":req.volume,"pitch":req.pitch},"audio_setting":{"format":"pcm","sample_rate":24000,"channel":1}});
                if socket.send(Message::Text(start.to_string())).await.is_err() {
                    send_error(&tx, Error::Network).await;
                    return;
                }
            }
            (1, "task_started") => {
                phase = 2;
                if tx
                    .send(Ok(Event::Ready {
                        sample_rate: 24000,
                        channels: 1,
                        format: PcmFormat::Pcm16,
                    }))
                    .await
                    .is_err()
                {
                    return;
                }
                for value in [
                    json!({"event":"task_continue","text":req.text}),
                    json!({"event":"task_finish"}),
                ] {
                    if socket.send(Message::Text(value.to_string())).await.is_err() {
                        send_error(&tx, Error::Network).await;
                        return;
                    }
                }
            }
            (2, "task_continued") => {
                let audio = v.pointer("/data/audio");
                if audio.is_some_and(|a| !a.is_string()) {
                    send_error(&tx, Error::Protocol).await;
                    return;
                }
                let h = audio.and_then(Value::as_str).unwrap_or("");
                if !h.len().is_multiple_of(2) || !h.bytes().all(|b| b.is_ascii_hexdigit()) {
                    send_error(&tx, Error::Protocol).await;
                    return;
                }
                let bytes = (0..h.len())
                    .step_by(2)
                    .map(|i| u8::from_str_radix(&h[i..i + 2], 16).expect("validated hex"))
                    .collect::<Vec<_>>();
                if !bytes.is_empty() && tx.send(Ok(Event::Audio(bytes))).await.is_err() {
                    return;
                }
            }
            (2, "sentence_start" | "sentence_end") => (),
            (2, "task_finished") => {
                let _ = tx.send(Ok(Event::Completed)).await;
                return;
            }
            _ => {
                send_error(&tx, Error::Protocol).await;
                return;
            }
        }
        deadline = tokio::time::Instant::now() + idle;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validates_utf16_and_numeric_bounds() {
        let mut r = SpeechRequest {
            id: 1,
            text: "hello".into(),
            voice_id: "v".into(),
            model: "m".into(),
            speed: 1.0,
            volume: 1.0,
            pitch: 0,
        };
        assert!(valid(&r));
        r.text = "😀".repeat(5000);
        assert!(!valid(&r));
        r.text = "ok".into();
        r.speed = f64::NAN;
        assert!(!valid(&r));
    }
}
