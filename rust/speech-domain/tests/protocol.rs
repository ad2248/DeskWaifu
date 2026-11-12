use deskwaifu_speech_domain::{synthesize, Error, Event, PcmFormat, SpeechRequest};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::time::Duration;
use tokio::{net::TcpListener, time::timeout};
use tokio_tungstenite::{accept_async, tungstenite::Message};

fn request() -> SpeechRequest {
    SpeechRequest {
        id: 42,
        text: "hello 🎙".into(),
        voice_id: "voice-test".into(),
        model: "model-test".into(),
        speed: 1.25,
        volume: 2.5,
        pitch: -3,
    }
}

async fn server() -> (String, TcpListener) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    (format!("ws://{address}"), listener)
}

async fn accept(
    stream: tokio::net::TcpStream,
) -> tokio_tungstenite::WebSocketStream<tokio::net::TcpStream> {
    accept_async(stream).await.unwrap()
}

async fn incoming(ws: &mut tokio_tungstenite::WebSocketStream<tokio::net::TcpStream>) -> Value {
    let message = ws.next().await.unwrap().unwrap();
    serde_json::from_str(message.to_text().unwrap()).unwrap()
}

async fn send(
    ws: &mut tokio_tungstenite::WebSocketStream<tokio::net::TcpStream>,
    event: &str,
    extra: Value,
) {
    let mut value = json!({"event": event, "base_resp": {"status_code": 0}});
    if let Some(map) = extra.as_object() {
        for (key, item) in map {
            value[key] = item.clone();
        }
    }
    ws.send(Message::Text(value.to_string())).await.unwrap();
}

#[tokio::test]
async fn sends_exact_protocol_settings_and_streams_pcm_chunks_in_order() {
    let (endpoint, listener) = server().await;
    let peer = tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let mut ws = accept(stream).await;
        send(&mut ws, "connected_success", json!({})).await;
        let start = incoming(&mut ws).await;
        assert_eq!(start["event"], "task_start");
        assert_eq!(start["model"], "model-test");
        assert_eq!(
            start["voice_setting"],
            json!({"voice_id":"voice-test","speed":1.25,"vol":2.5,"pitch":-3})
        );
        assert_eq!(
            start["audio_setting"],
            json!({"format":"pcm","sample_rate":24000,"channel":1})
        );
        send(&mut ws, "task_started", json!({})).await;
        let continuation = incoming(&mut ws).await;
        let finish = incoming(&mut ws).await;
        assert_eq!(
            continuation,
            json!({"event":"task_continue","text":"hello 🎙"})
        );
        assert_eq!(finish, json!({"event":"task_finish"}));
        send(&mut ws, "sentence_start", json!({})).await;
        send(
            &mut ws,
            "task_continued",
            json!({"data":{"audio":"000102"}}),
        )
        .await;
        send(&mut ws, "task_continued", json!({"data":{"audio":"03"}})).await;
        send(&mut ws, "sentence_end", json!({})).await;
        send(&mut ws, "task_finished", json!({})).await;
    });
    let mut events = synthesize(
        endpoint,
        "local-test-key",
        request(),
        Duration::from_secs(2),
    )
    .await;
    assert_eq!(
        events.recv().await.unwrap().unwrap(),
        Event::Ready {
            sample_rate: 24000,
            channels: 1,
            format: PcmFormat::Pcm16
        }
    );
    assert_eq!(
        events.recv().await.unwrap().unwrap(),
        Event::Audio(vec![0, 1, 2])
    );
    assert_eq!(events.recv().await.unwrap().unwrap(), Event::Audio(vec![3]));
    assert_eq!(events.recv().await.unwrap().unwrap(), Event::Completed);
    assert!(events.recv().await.is_none());
    peer.await.unwrap();
}

async fn single_error(response: &str) -> Error {
    let (endpoint, listener) = server().await;
    let response = response.to_owned();
    tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let mut ws = accept(stream).await;
        ws.send(Message::Text(response)).await.unwrap();
    });
    let mut events = synthesize(
        endpoint,
        "not-a-real-credential",
        request(),
        Duration::from_secs(2),
    )
    .await;
    events.recv().await.unwrap().unwrap_err()
}

#[tokio::test]
async fn rejects_malformed_messages_event_order_and_bad_hex() {
    assert_eq!(single_error("not-json").await, Error::Protocol);
    assert_eq!(
        single_error(r#"{"event":"connected_success","base_resp":{"status_code":"0"}}"#).await,
        Error::Protocol
    );
    assert_eq!(
        single_error(r#"{"event":"task_finished","base_resp":{"status_code":0}}"#).await,
        Error::Protocol
    );

    for audio in [json!("a"), json!("xz"), json!(17), json!("你好")] {
        let (endpoint, listener) = server().await;
        tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            let mut ws = accept(stream).await;
            send(&mut ws, "connected_success", json!({})).await;
            incoming(&mut ws).await;
            send(&mut ws, "task_started", json!({})).await;
            incoming(&mut ws).await;
            incoming(&mut ws).await;
            send(&mut ws, "task_continued", json!({"data":{"audio":audio}})).await;
        });
        let mut events = synthesize(endpoint, "key", request(), Duration::from_secs(2)).await;
        assert!(matches!(
            events.recv().await.unwrap().unwrap(),
            Event::Ready { .. }
        ));
        assert_eq!(events.recv().await.unwrap().unwrap_err(), Error::Protocol);
    }
}

#[tokio::test]
async fn maps_authentication_and_synthesis_status_codes_without_echoing_server_text() {
    assert_eq!(single_error(r#"{"event":"error","base_resp":{"status_code":1004,"status_msg":"secret-looking response"}}"#).await, Error::Authentication);
    assert_eq!(
        single_error(
            r#"{"event":"error","base_resp":{"status_code":2013,"status_msg":"private"}}"#
        )
        .await,
        Error::Synthesis(2013)
    );
}

#[tokio::test]
async fn rejects_insecure_remote_websocket_endpoints() {
    let mut events = synthesize(
        "ws://speech.example.test/ws",
        "key",
        request(),
        Duration::from_secs(1),
    )
    .await;
    assert_eq!(
        events.recv().await.unwrap().unwrap_err(),
        Error::InvalidRequest
    );
}

#[tokio::test]
async fn rejects_messages_over_limit_in_websocket_decoder() {
    let (endpoint, listener) = server().await;
    tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let mut ws = accept(stream).await;
        // The configured client decoder must reject this before constructing a text String.
        let _ = ws
            .send(Message::Text("x".repeat(2 * 1024 * 1024 + 1)))
            .await;
    });
    let mut events = synthesize(endpoint, "key", request(), Duration::from_secs(2)).await;
    assert!(matches!(events.recv().await.unwrap(), Err(Error::Network)));
}

#[tokio::test]
async fn reports_idle_timeout() {
    let (endpoint, listener) = server().await;
    tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let _ws = accept(stream).await;
        tokio::time::sleep(Duration::from_millis(300)).await;
    });
    let mut events = synthesize(endpoint, "key", request(), Duration::from_millis(40)).await;
    assert_eq!(
        timeout(Duration::from_secs(1), events.recv())
            .await
            .unwrap()
            .unwrap()
            .unwrap_err(),
        Error::Timeout
    );
}

#[tokio::test]
async fn dropping_event_receiver_cancels_socket_task() {
    let (endpoint, listener) = server().await;
    let (closed_tx, closed_rx) = tokio::sync::oneshot::channel();
    tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let mut ws = accept(stream).await;
        let _ = closed_tx.send(timeout(Duration::from_secs(1), ws.next()).await.is_ok());
    });
    let events = synthesize(endpoint, "key", request(), Duration::from_secs(10)).await;
    drop(events);
    assert!(closed_rx.await.unwrap());
}
