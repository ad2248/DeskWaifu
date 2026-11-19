use super::*;
use hickory_resolver::{
    config::{NameServerConfig, ResolveHosts, ResolverConfig},
    name_server::TokioConnectionProvider,
    proto::{
        op::{Message as DnsMessage, MessageType},
        rr::{RData, Record, rdata::A},
        xfer::Protocol,
    },
};
use std::{net::UdpSocket, sync::mpsc, thread};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    net::TcpListener,
};
use tokio_tungstenite::{WebSocketStream, accept_async};

#[cfg(target_os = "linux")]
#[path = "tls_tests.rs"]
mod tls_tests;

fn config(endpoint: Url, model: &str) -> Config {
    Config {
        endpoint,
        model: model.into(),
        max_recording_seconds: 1,
        authorization: Some("Bearer test-key".parse().unwrap()),
    }
}

async fn send_event<S: AsyncRead + AsyncWrite + Unpin>(
    socket: &mut WebSocketStream<S>,
    id: &str,
    name: &str,
    payload: Value,
) {
    socket
        .send(Message::Text(
            json!({"header": {"task_id": id, "event": name}, "payload": payload})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
}

async fn read_run<S: AsyncRead + AsyncWrite + Unpin>(socket: &mut WebSocketStream<S>) -> String {
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
        streaming(
            config(url, "test"),
            vec![7; 2400],
            Dns::default(),
            START_TIMEOUT
        ),
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
            streaming(
                config(url, "test"),
                vec![7; 2],
                Dns::default(),
                START_TIMEOUT
            ),
            mock
        );
        assert_eq!(actual.unwrap_err(), expected);
    }
}

async fn read_http_request(stream: &mut (impl AsyncRead + Unpin)) -> (String, Value) {
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
            assert_eq!(&wav[4..8], &((wav.len() - 8) as u32).to_le_bytes());
            assert_eq!(&wav[24..28], &16_000u32.to_le_bytes());
            assert_eq!(&wav[36..40], b"data");
            assert_eq!(&wav[40..44], &((pcm.len() + 8000) as u32).to_le_bytes());
            assert_eq!(&wav[44..44 + pcm.len()], &pcm);
            assert_eq!(&wav[44 + pcm.len()..], &[0; 8000]);
            let body = expected_body.to_string();
            stream.write_all(format!("HTTP/1.1 {status}\r\nLocation: http://{addr}/redirected\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len()).as_bytes()).await.unwrap();
        };
        let client = https_client(Dns::default(), START_TIMEOUT).no_proxy();
        let (actual, ()) = timeout(Duration::from_secs(3), async {
            tokio::join!(https(config, url, compatible, &pcm, client), mock)
        })
        .await
        .unwrap();
        assert_eq!(actual.as_deref().map_err(|error| *error), expected);
    }
}

// 测试目的：验证旧模型的录音与尾静音合计不超过五分钟。
// 测试方法：按 300 秒配置预留静音时间，并用环回服务解码含多余尾块的上传请求，核对 WAV 帧数和内容。
#[tokio::test]
async fn legacy_https_wav_stays_within_five_minutes() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let mut config = config(
        Url::parse(&format!("https://{addr}/v1")).unwrap(),
        LEGACY_MODEL,
    );
    config.max_recording_seconds = 300;
    assert_eq!(recording_duration(&config), Duration::from_millis(299_750));
    let (mut url, compatible) = http_endpoint(config.endpoint.clone(), &config.model);
    url.set_scheme("http").unwrap();
    let pcm = vec![7; 9_600_000];
    let mock = async {
        let (mut stream, _) = listener.accept().await.unwrap();
        let (_, body) = read_http_request(&mut stream).await;
        let data = body["input"]["messages"][0]["content"][0]["input_audio"]["data"]
            .as_str()
            .unwrap()
            .strip_prefix("data:audio/wav;base64,")
            .unwrap();
        let wav = STANDARD.decode(data).unwrap();
        assert_eq!(wav.len() - 44, 300 * 16_000 * 2);
        assert_eq!(&wav[40..44], &9_600_000u32.to_le_bytes());
        let tail_start = wav.len() - 8000;
        assert_eq!(&wav[44..tail_start], &pcm[..9_592_000]);
        assert_eq!(&wav[tail_start..], &[0; 8000]);
        let response = r#"{"text":"ok"}"#;
        stream
            .write_all(
                format!(
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{response}",
                    response.len()
                )
                .as_bytes(),
            )
            .await
            .unwrap();
    };
    let client = https_client(Dns::default(), START_TIMEOUT).no_proxy();
    let (actual, ()) = timeout(Duration::from_secs(15), async {
        tokio::join!(https(config, url, compatible, &pcm, client), mock)
    })
    .await
    .unwrap();
    assert_eq!(actual.as_deref(), Ok("ok"));
}

const STALLED_HOST: &str = "voice-stalled.example.org.";
const TEST_WAIT: Duration = Duration::from_secs(3);

// 测试目的：确认 HTTPS 保留 HTTP 代理代解析的行为，并将识别服务原主机名传给代理。
// 测试方法：由环回 DNS 只回答代理域名，检查环回代理收到指向原识别服务的 CONNECT。
#[tokio::test]
async fn https_proxy_resolves_locally_and_receives_the_original_target() {
    const PROXY_HOST: &str = "voice-proxy.example.org.";
    let (dns_socket, dns) = stalled_dns();
    dns_socket.set_nonblocking(true).unwrap();
    let dns_socket = tokio::net::UdpSocket::from_std(dns_socket).unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let proxy = reqwest::Proxy::https(format!(
        "http://{PROXY_HOST}:{}",
        listener.local_addr().unwrap().port()
    ))
    .unwrap();
    let client = https_client(dns, START_TIMEOUT).proxy(proxy);
    let url = Url::parse(&format!("https://{STALLED_HOST}/v1/chat/completions")).unwrap();
    let config = config(url.clone(), "test");
    let mock = async {
        let mut packet = [0; 512];
        let (len, source) = dns_socket.recv_from(&mut packet).await.unwrap();
        let mut message = DnsMessage::from_vec(&packet[..len]).unwrap();
        let name = message.queries()[0].name().clone();
        assert_eq!(name.to_utf8(), PROXY_HOST);
        message
            .set_message_type(MessageType::Response)
            .add_answer(Record::from_rdata(name, 60, RData::A(A::new(127, 0, 0, 1))));
        dns_socket
            .send_to(&message.to_vec().unwrap(), source)
            .await
            .unwrap();

        let (mut stream, _) = listener.accept().await.unwrap();
        let expected = format!("CONNECT {STALLED_HOST}:443 HTTP/1.1\r\n");
        let mut first_line = vec![0; expected.len()];
        stream.read_exact(&mut first_line).await.unwrap();
        assert_eq!(first_line, expected.as_bytes());
    };
    let (result, ()) = timeout(TEST_WAIT, async {
        tokio::join!(https(config, url, true, &[0; 2], client), mock)
    })
    .await
    .unwrap();
    assert_eq!(result, Err("asr_network"));
}

fn stalled_dns() -> (UdpSocket, Dns) {
    let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
    socket.set_read_timeout(Some(TEST_WAIT)).unwrap();
    let nameserver = NameServerConfig::new(socket.local_addr().unwrap(), Protocol::Udp);
    let config = ResolverConfig::from_parts(None, vec![], vec![nameserver]);
    let mut resolver =
        TokioResolver::builder_with_config(config, TokioConnectionProvider::default());
    let options = resolver.options_mut();
    options.timeout = Duration::from_secs(60);
    options.ip_strategy = LookupIpStrategy::Ipv4Only;
    options.use_hosts_file = ResolveHosts::Never;
    let dns = Dns(Arc::new(OnceCell::with_value(resolver.build())));
    (socket, dns)
}

fn wait_for_dns_query(socket: &UdpSocket, host: &str) {
    let mut packet = [0; 512];
    let (len, _) = socket
        .recv_from(&mut packet)
        .expect("ASR did not reach DNS");
    let message = DnsMessage::from_vec(&packet[..len]).unwrap();
    assert_eq!(message.queries()[0].name().to_utf8(), host);
}

fn dns_worker(scheme: &str, dns: Dns, start_timeout: Duration) -> crate::Worker<Result<String>> {
    let url = Url::parse(&format!("{scheme}://{STALLED_HOST}/v1")).unwrap();
    let config = config(url.clone(), "test");
    crate::recognition_worker(async move {
        if url.scheme() == "wss" {
            streaming(config, vec![0; 2], dns, start_timeout).await
        } else {
            let client = https_client(dns, start_timeout).no_proxy();
            https(config, url, true, &[0; 2], client).await
        }
    })
    .unwrap()
}

fn before_test_deadline<T: Send + 'static>(work: impl FnOnce() -> T + Send + 'static) -> T {
    let (done, result) = mpsc::channel();
    let thread = thread::spawn(move || done.send(work()).unwrap());
    let value = result
        .recv_timeout(TEST_WAIT)
        .expect("ASR worker did not join before the deadline");
    thread.join().unwrap();
    value
}

// 测试目的：确认宿主丢弃识别任务时，WSS 和 HTTPS 的慢 DNS 不会拖住工作线程退出。
// 测试方法：等待环回 DNS 收到查询，保持套接字开放且不响应，然后丢弃工作线程句柄并检查 join 及时完成。
#[test]
fn worker_cancellation_joins_while_dns_is_pending() {
    for scheme in ["wss", "https"] {
        let (socket, dns) = stalled_dns();
        let worker = dns_worker(scheme, dns, START_TIMEOUT);
        wait_for_dns_query(&socket, STALLED_HOST);
        before_test_deadline(move || drop(worker));
    }
}

// 测试目的：确认连接期限包含 WSS 和 HTTPS 的 DNS，并且工作线程及时上报 asr_timeout。
// 测试方法：让环回 DNS 收到查询后保持沉默，设置比 DNS 自身期限短的连接期限，在外层时限内等待工作线程结果。
#[test]
fn worker_reports_connection_timeout_while_dns_is_pending() {
    for scheme in ["wss", "https"] {
        let (socket, dns) = stalled_dns();
        let worker = dns_worker(scheme, dns, Duration::from_millis(500));
        wait_for_dns_query(&socket, STALLED_HOST);
        let result = before_test_deadline(move || {
            tokio::runtime::Builder::new_current_thread()
                .build()
                .unwrap()
                .block_on(worker.result())
        });
        assert_eq!(result.unwrap(), Err("asr_timeout"), "{scheme}");
    }
}
