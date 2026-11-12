use deskwaifu_asr_domain::{recognize_stream, AsrError};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::time::Duration;
use tokio::net::TcpListener;
use tokio_tungstenite::{
    accept_hdr_async,
    tungstenite::{
        handshake::server::{Request, Response},
        Message,
    },
};

async fn server() -> (String, TcpListener) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    (format!("ws://{}", listener.local_addr().unwrap()), listener)
}
#[allow(clippy::result_large_err)]
async fn accept(
    listener: TcpListener,
    expected_auth: Option<&'static str>,
) -> tokio_tungstenite::WebSocketStream<tokio::net::TcpStream> {
    let (stream, _) = listener.accept().await.unwrap();
    accept_hdr_async(stream, move |req: &Request, response: Response| {
        if let Some(expected) = expected_auth {
            assert_eq!(req.headers().get("authorization").unwrap(), expected);
        }
        Ok(response)
    })
    .await
    .unwrap()
}
async fn receive(ws: &mut tokio_tungstenite::WebSocketStream<tokio::net::TcpStream>) -> Message {
    ws.next().await.unwrap().unwrap()
}
async fn event(
    ws: &mut tokio_tungstenite::WebSocketStream<tokio::net::TcpStream>,
    id: &str,
    name: &str,
    payload: Value,
) {
    ws.send(Message::Text(
        json!({"header":{"task_id":id,"event":name},"payload":payload}).to_string(),
    ))
    .await
    .unwrap();
}

#[tokio::test]
async fn exact_run_pcm_chunks_finish_and_final_sentence_concatenation() {
    let (url, listener) = server().await;
    let peer = tokio::spawn(async move {
        let mut ws = accept(listener, Some("Bearer local-key")).await;
        let request: Value =
            serde_json::from_str(receive(&mut ws).await.to_text().unwrap()).unwrap();
        let h = request["header"].clone();
        assert_eq!(h["action"], "run-task");
        assert_eq!(h["streaming"], "duplex");
        let id = h["task_id"].as_str().unwrap().to_owned();
        assert_eq!(uuid::Uuid::parse_str(&id).unwrap().to_string(), id);
        assert_eq!(
            request["payload"],
            json!({"task_group":"audio","task":"asr","function":"recognition","model":"asr-test","parameters":{"format":"pcm","sample_rate":16000},"input":{}})
        );
        event(&mut ws, &id, "task-started", json!({})).await;
        let first = receive(&mut ws).await;
        assert_eq!(first, Message::Binary(vec![3; 3200]));
        let second = receive(&mut ws).await;
        assert_eq!(second, Message::Binary(vec![4; 800]));
        let finish: Value =
            serde_json::from_str(receive(&mut ws).await.to_text().unwrap()).unwrap();
        assert_eq!(
            finish,
            json!({"header":{"action":"finish-task","task_id":id,"streaming":"duplex"},"payload":{"input":{}}})
        );
        event(
            &mut ws,
            &id,
            "result-generated",
            json!({"output":{"sentence":{"heartbeat":true}}}),
        )
        .await;
        event(
            &mut ws,
            &id,
            "result-generated",
            json!({"output":{"sentence":{"text":"discard","sentence_end":false}}}),
        )
        .await;
        event(
            &mut ws,
            &id,
            "result-generated",
            json!({"output":{"sentence":{"text":"Hello ","sentence_end":true}}}),
        )
        .await;
        event(
            &mut ws,
            &id,
            "result-generated",
            json!({"output":{"sentence":{"text":"world","sentence_end":true}}}),
        )
        .await;
        event(&mut ws, &id, "task-finished", json!({})).await;
    });
    let mut pcm = vec![3; 3200];
    pcm.extend(vec![4; 800]);
    assert_eq!(
        recognize_stream(url, "local-key", "asr-test", &pcm, Duration::from_secs(5))
            .await
            .unwrap(),
        "Hello world"
    );
    peer.await.unwrap();
}

#[tokio::test]
async fn rejects_bad_input_endpoint_and_protocol_events() {
    assert_eq!(
        recognize_stream("wss://example.com", "key", "m", &[], Duration::from_secs(1)).await,
        Err(AsrError::NoAudio)
    );
    assert_eq!(
        recognize_stream(
            "wss://example.com",
            "key",
            "m",
            &[1],
            Duration::from_secs(1)
        )
        .await,
        Err(AsrError::InvalidAudio)
    );
    assert_eq!(
        recognize_stream(
            "ws://example.com",
            "key",
            "m",
            &[1, 2],
            Duration::from_secs(1)
        )
        .await,
        Err(AsrError::InvalidEndpoint)
    );
    let (url, listener) = server().await;
    let peer = tokio::spawn(async move {
        let mut ws = accept(listener, None).await;
        let _: Value = serde_json::from_str(receive(&mut ws).await.to_text().unwrap()).unwrap();
        event(&mut ws, "not-the-task", "task-started", json!({})).await;
    });
    assert_eq!(
        recognize_stream(url, "key", "m", &[0, 0], Duration::from_secs(2)).await,
        Err(AsrError::Protocol)
    );
    peer.await.unwrap();
}

#[tokio::test]
async fn malformed_duplicate_order_no_speech_and_text_limit_fail() {
    for (event_name, payload, expected) in [
        (
            "result-generated",
            json!({"output":{"sentence":{"text":"x","sentence_end":true}}}),
            AsrError::Protocol,
        ),
        ("task-finished", json!({}), AsrError::Protocol),
    ] {
        let (url, listener) = server().await;
        let name = event_name.to_owned();
        tokio::spawn(async move {
            let mut ws = accept(listener, None).await;
            let req: Value =
                serde_json::from_str(receive(&mut ws).await.to_text().unwrap()).unwrap();
            event(
                &mut ws,
                req["header"]["task_id"].as_str().unwrap(),
                &name,
                payload,
            )
            .await;
        });
        assert_eq!(
            recognize_stream(url, "key", "m", &[0, 0], Duration::from_secs(2)).await,
            Err(expected)
        );
    }

    let (url, listener) = server().await;
    tokio::spawn(async move {
        let mut ws = accept(listener, None).await;
        let req: Value = serde_json::from_str(receive(&mut ws).await.to_text().unwrap()).unwrap();
        let id = req["header"]["task_id"].as_str().unwrap().to_owned();
        event(&mut ws, &id, "task-started", json!({})).await;
        let _ = receive(&mut ws).await;
        let _ = receive(&mut ws).await;
        event(
            &mut ws,
            &id,
            "result-generated",
            json!({"output":{"sentence":{"text":"x".repeat(600_000),"sentence_end":true}}}),
        )
        .await;
        event(
            &mut ws,
            &id,
            "result-generated",
            json!({"output":{"sentence":{"text":"y".repeat(600_000),"sentence_end":true}}}),
        )
        .await;
    });
    assert_eq!(
        recognize_stream(url, "key", "m", &[0, 0], Duration::from_secs(3)).await,
        Err(AsrError::ResponseTooLarge)
    );
}

#[tokio::test]
async fn timeout_and_remote_failure_are_generic() {
    let (url, listener) = server().await;
    let peer = tokio::spawn(async move {
        let _ws = accept(listener, None).await;
        tokio::time::sleep(Duration::from_millis(250)).await;
    });
    assert_eq!(
        recognize_stream(url, "key", "m", &[0, 0], Duration::from_millis(50)).await,
        Err(AsrError::Timeout)
    );
    peer.await.unwrap();

    let (url, listener) = server().await;
    let peer = tokio::spawn(async move {
        let mut ws = accept(listener, None).await;
        let req: Value = serde_json::from_str(receive(&mut ws).await.to_text().unwrap()).unwrap();
        event(
            &mut ws,
            req["header"]["task_id"].as_str().unwrap(),
            "task-failed",
            json!({"secret":"not exposed"}),
        )
        .await;
    });
    assert_eq!(
        recognize_stream(url, "key", "m", &[0, 0], Duration::from_secs(2)).await,
        Err(AsrError::Remote)
    );
    peer.await.unwrap();
}
