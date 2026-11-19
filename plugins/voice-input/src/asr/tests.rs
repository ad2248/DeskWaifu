use super::*;
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::{TcpListener, TcpStream},
};
use tokio_tungstenite::{WebSocketStream, accept_async};

fn config(endpoint: Url, model: &str) -> Config {
    Config {
        endpoint,
        model: model.into(),
        max_recording_seconds: 1,
        authorization: Some("Bearer test-key".parse().unwrap()),
    }
}

async fn send_event(socket: &mut WebSocketStream<TcpStream>, id: &str, name: &str, payload: Value) {
    socket
        .send(Message::Text(
            json!({"header": {"task_id": id, "event": name}, "payload": payload})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
}

async fn read_run(socket: &mut WebSocketStream<TcpStream>) -> String {
    let message = socket.next().await.unwrap().unwrap();
    let request: Value = serde_json::from_str(message.to_text().unwrap()).unwrap();
    assert_eq!(request["header"]["action"], "run-task");
    assert_eq!(
        request["payload"]["parameters"],
        json!({"format": "pcm", "sample_rate": 16000})
    );
    request["header"]["task_id"].as_str().unwrap().into()
}

// 测试目的：确认流式识别按约定分块上传 PCM 和尾静音，并保留最终句的原始空白。
// 测试方法：用暂停的 Tokio 时钟驱动环回 WebSocket，核对任务握手、帧大小、帧间隔和尾静音，再用心跳、中间和最终结果验证转录。
#[tokio::test(start_paused = true)]
async fn streaming_uploads_pcm_and_silence_and_joins_only_final_sentences() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = Url::parse(&format!(
        "ws://{}/api-ws/v1/inference",
        listener.local_addr().unwrap()
    ))
    .unwrap();
    let mock = async move {
        let mut socket = accept_async(listener.accept().await.unwrap().0)
            .await
            .unwrap();
        let id = read_run(&mut socket).await;
        send_event(&mut socket, &id, "task-started", json!({})).await;
        for sentence in [
            json!({"heartbeat": true}),
            json!({"text":"part", "sentence_end": false}),
            json!({"text":" 中文", "sentence_end": true}),
        ] {
            send_event(
                &mut socket,
                &id,
                "result-generated",
                json!({"output": {"sentence": sentence}}),
            )
            .await;
        }
        let mut frames = Vec::new();
        let mut last_frame = None;
        loop {
            match socket.next().await.unwrap().unwrap() {
                Message::Binary(chunk) => {
                    let now = Instant::now();
                    if let Some(last_frame) = last_frame {
                        assert!(now - last_frame >= PACKET_INTERVAL);
                    }
                    last_frame = Some(now);
                    frames.push(chunk);
                }
                Message::Text(text) => {
                    let value: Value = serde_json::from_str(&text).unwrap();
                    assert_eq!(value["header"]["action"], "finish-task");
                    assert_eq!(value["header"]["task_id"], id);
                    break;
                }
                unexpected => panic!("unexpected message: {unexpected:?}"),
            }
        }
        assert_eq!(
            frames.iter().map(|frame| frame.len()).collect::<Vec<_>>(),
            [3200, 3200, 3200, 800]
        );
        assert_eq!(&frames.concat()[..2400], &[7; 2400]);
        assert_eq!(&frames.concat()[2400..], &[0; 8000]);
        send_event(
            &mut socket,
            &id,
            "result-generated",
            json!({"output": {"sentence": {"text":"\n尾句 ", "sentence_end": true}}}),
        )
        .await;
        send_event(&mut socket, &id, "task-finished", json!({})).await;
    };
    let (actual, ()) = tokio::join!(
        streaming(config(url, "test"), vec![7; 2400], START_TIMEOUT),
        mock
    );
    assert_eq!(actual.unwrap(), " 中文\n尾句 ");
}

// 测试目的：确认流式识别拒绝任务 ID 不符或顺序错误的事件，并区分任务失败与启动超时。
// 测试方法：让环回 WebSocket 分别返回错误 ID、提前完成、任务失败或保持沉默，用暂停的时钟检查各场景的错误码。
#[tokio::test(start_paused = true)]
async fn streaming_rejects_invalid_events_and_times_out() {
    for (name, wrong_id, expected) in [
        (Some("task-started"), true, "asr_protocol"),
        (Some("task-finished"), false, "asr_protocol"),
        (Some("task-failed"), false, "asr_task_failed"),
        (None, false, "asr_timeout"),
    ] {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = Url::parse(&format!("ws://{}", listener.local_addr().unwrap())).unwrap();
        let mock = async move {
            let mut socket = accept_async(listener.accept().await.unwrap().0)
                .await
                .unwrap();
            let id = read_run(&mut socket).await;
            if let Some(name) = name {
                send_event(
                    &mut socket,
                    if wrong_id { "other" } else { &id },
                    name,
                    json!({}),
                )
                .await;
            } else {
                assert!(socket.next().await.is_none_or(|message| message.is_err()));
            }
        };
        let (actual, ()) = tokio::join!(
            streaming(config(url, "test"), vec![7; 2], START_TIMEOUT),
            mock
        );
        assert_eq!(actual.unwrap_err(), expected);
    }
}

async fn read_http_request(stream: &mut TcpStream) -> (String, Value) {
    let mut request = Vec::new();
    let mut buffer = [0; 4096];
    loop {
        let count = stream.read(&mut buffer).await.unwrap();
        assert!(count != 0);
        request.extend_from_slice(&buffer[..count]);
        if let Some(pos) = request.windows(4).position(|part| part == b"\r\n\r\n") {
            let start = pos + 4;
            let header = std::str::from_utf8(&request[..pos]).unwrap();
            let len: usize = header
                .lines()
                .find_map(|line| {
                    let (key, value) = line.split_once(':')?;
                    key.eq_ignore_ascii_case("content-length")
                        .then(|| value.trim().parse().unwrap())
                })
                .unwrap();
            if request.len() >= start + len {
                return (
                    header.to_owned(),
                    serde_json::from_slice(&request[start..start + len]).unwrap(),
                );
            }
        }
    }
}

// 测试目的：确认两种旧版 ASR 请求使用正确的路径、请求体和响应字段，并拒绝重定向。
// 测试方法：先解析 HTTPS 路径并检查域名，再在环回 HTTP 服务上检查请求头、JSON 和 WAV 内容，分别返回两种成功响应和 307。
#[tokio::test]
async fn http_requests_resolve_before_upload_and_reject_redirects() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let pcm = [1, 2, 3, 4];
    for (model, expected_path, expected_body, status, expected) in [
        (
            "compatible",
            "/compatible-mode/v1/chat/completions",
            json!({"choices":[{"message":{"content":"  文本\n"}}]}),
            "200 OK",
            Ok("  文本\n"),
        ),
        (
            "qwen-audio-3.0-asr-flash",
            NATIVE_PATH,
            json!({"text":"  首选\n", "output":{"text":"other"}}),
            "200 OK",
            Ok("  首选\n"),
        ),
        (
            "compatible",
            "/compatible-mode/v1/chat/completions",
            json!({}),
            "307 Temporary Redirect",
            Err("asr_http"),
        ),
    ] {
        let config = config(
            Url::parse(&format!("https://{addr}/compatible-mode/v1/")).unwrap(),
            model,
        );
        let (mut url, compatible) = http_endpoint(config.endpoint.clone(), &config.model);
        assert_eq!(url.path(), expected_path);
        assert_eq!(url.host_str(), config.endpoint.host_str());
        url.set_scheme("http").unwrap();
        let mock = async {
            let (mut stream, _) = listener.accept().await.unwrap();
            let (header, body) = read_http_request(&mut stream).await;
            assert!(header.starts_with(&format!("POST {expected_path} HTTP/1.1")));
            assert!(header.contains("authorization: Bearer test-key"));
            let messages = if compatible {
                assert_eq!(body["stream"], false);
                &body["messages"]
            } else {
                assert!(header.contains("x-dashscope-sse: disable"));
                assert_eq!(body["parameters"]["sample_rate"], "16000");
                &body["input"]["messages"]
            };
            let data = messages[0]["content"][0]["input_audio"]["data"]
                .as_str()
                .unwrap();
            let wav = STANDARD
                .decode(data.strip_prefix("data:audio/wav;base64,").unwrap())
                .unwrap();
            assert_eq!(&wav[..4], b"RIFF");
            assert_eq!(&wav[24..28], &16_000u32.to_le_bytes());
            assert_eq!(&wav[44..], &pcm);
            let body = expected_body.to_string();
            stream.write_all(format!("HTTP/1.1 {status}\r\nLocation: http://{addr}/redirected\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len()).as_bytes()).await.unwrap();
        };
        let (actual, ()) = timeout(Duration::from_secs(3), async {
            tokio::join!(https(config, url, compatible, &pcm), mock)
        })
        .await
        .unwrap();
        assert_eq!(actual.as_deref().map_err(|error| *error), expected);
    }
}
