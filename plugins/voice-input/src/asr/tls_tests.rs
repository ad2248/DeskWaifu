use super::{config, read_http_request, read_run, send_event};
use crate::asr::{Message, Url, recognize, timeout};
use futures_util::StreamExt;
use rcgen::{BasicConstraints, Certificate, CertificateParams, IsCa, KeyPair, KeyUsagePurpose};
use serde_json::{Value, json};
use std::{env, fs, path::Path, process::Command, sync::Arc, time::Duration};
use tokio::{io::AsyncWriteExt, net::TcpListener};
use tokio_rustls::{
    TlsAcceptor,
    rustls::{self, AlertDescription, ServerConfig, pki_types::PrivatePkcs8KeyDer},
};
use tokio_tungstenite::accept_async;

const CHILD_CA_FILE: &str = "DESKWAIFU_TEST_SYSTEM_CA_FILE";
const LOOPBACK: &str = "127.0.0.1";
const TRANSCRIPT: &str = "私有 CA";

// 测试目的：确认 HTTPS 可以使用系统信任的私有 CA，同时校验签发链和请求地址。
// 测试方法：在独立进程中用 SSL_CERT_FILE 指定临时信任库，访问环回 TLS 服务，依次验证空信任库、导入 CA 和地址不匹配。
#[test]
fn https_uses_system_roots() {
    with_system_roots("https", "asr::tests::tls_tests::https_uses_system_roots");
}

// 测试目的：确认 WSS 可以使用系统信任的私有 CA，同时校验签发链和请求地址。
// 测试方法：在独立进程中用 SSL_CERT_FILE 指定临时信任库，通过环回 TLS 服务完成 WebSocket 识别，并检查不受信和地址不匹配时的 TLS 拒绝。
#[test]
fn wss_uses_system_roots() {
    with_system_roots("wss", "asr::tests::tls_tests::wss_uses_system_roots");
}

fn with_system_roots(scheme: &str, test: &str) {
    if let Some(path) = env::var_os(CHILD_CA_FILE) {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap()
            .block_on(check_system_roots(scheme, Path::new(&path)));
        return;
    }

    // 子进程负责读取临时信任库，避免并行测试共用进程环境变量。
    let trust_dir = tempfile::tempdir().unwrap();
    let ca_file = trust_dir.path().join("ca.pem");
    let output = Command::new(env::current_exe().unwrap())
        .args(["--exact", test, "--nocapture"])
        .env(CHILD_CA_FILE, &ca_file)
        .env("SSL_CERT_FILE", &ca_file)
        .env("SSL_CERT_DIR", trust_dir.path())
        .env("NO_PROXY", "*")
        .env("no_proxy", "*")
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(
        output.status.success() && stdout.contains("1 passed"),
        "{scheme} child test failed: {stdout}\n{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

async fn check_system_roots(scheme: &str, ca_file: &Path) {
    let mut ca_params = CertificateParams::new(Vec::new()).unwrap();
    ca_params.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);
    ca_params.key_usages = vec![KeyUsagePurpose::KeyCertSign];
    let ca_key = KeyPair::generate().unwrap();
    let ca = ca_params.self_signed(&ca_key).unwrap();
    let server = tls_server(LOOPBACK, &ca, &ca_key);

    fs::write(ca_file, "").unwrap();
    check_endpoint(scheme, server.clone(), Some(AlertDescription::UnknownCA)).await;
    fs::write(ca_file, ca.pem()).unwrap();
    check_endpoint(scheme, server, None).await;
    check_endpoint(
        scheme,
        tls_server("127.0.0.2", &ca, &ca_key),
        Some(AlertDescription::BadCertificate),
    )
    .await;
}

fn tls_server(ip: &str, ca: &Certificate, ca_key: &KeyPair) -> TlsAcceptor {
    let key = KeyPair::generate().unwrap();
    let cert = CertificateParams::new(vec![ip.to_owned()])
        .unwrap()
        .signed_by(&key, ca, ca_key)
        .unwrap();
    let server = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(
            vec![cert.der().clone()],
            PrivatePkcs8KeyDer::from(key.serialize_der()).into(),
        )
        .unwrap();
    TlsAcceptor::from(Arc::new(server))
}

async fn check_endpoint(scheme: &str, server: TlsAcceptor, rejection: Option<AlertDescription>) {
    let listener = TcpListener::bind((LOOPBACK, 0)).await.unwrap();
    let addr = listener.local_addr().unwrap();
    let url = Url::parse(&format!("{scheme}://{addr}/v1")).unwrap();
    let mock = async move {
        let (tcp, _) = listener.accept().await.unwrap();
        let tls = server.accept(tcp).await;
        if let Some(alert) = rejection {
            let error = tls.unwrap_err();
            assert_eq!(
                error.get_ref().and_then(|err| err.downcast_ref()),
                Some(&rustls::Error::AlertReceived(alert)),
                "{scheme}: {error}"
            );
            return;
        }
        let mut stream = tls.expect("client must trust the private CA from the native root store");
        if scheme == "https" {
            read_http_request(&mut stream).await;
            let body = json!({"choices": [{"message": {"content": TRANSCRIPT}}]}).to_string();
            stream
                .write_all(
                    format!(
                        "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                        body.len()
                    )
                    .as_bytes(),
                )
                .await
                .unwrap();
        } else {
            let mut socket = accept_async(stream).await.unwrap();
            let id = read_run(&mut socket).await;
            send_event(&mut socket, &id, "task-started", json!({})).await;
            loop {
                match socket.next().await.unwrap().unwrap() {
                    Message::Binary(_) => continue,
                    Message::Text(text) => {
                        let request: Value = serde_json::from_str(&text).unwrap();
                        assert_eq!(request["header"]["action"], "finish-task");
                        break;
                    }
                    message => panic!("unexpected {scheme} request: {message:?}"),
                }
            }
            send_event(
                &mut socket,
                &id,
                "result-generated",
                json!({"output": {"sentence": {"text": TRANSCRIPT, "sentence_end": true}}}),
            )
            .await;
            send_event(&mut socket, &id, "task-finished", json!({})).await;
        }
    };
    let (actual, ()) = timeout(Duration::from_secs(5), async {
        tokio::join!(recognize(config(url, "test"), vec![0; 2]), mock)
    })
    .await
    .expect("loopback TLS exchange timed out");
    assert_eq!(
        actual.as_deref(),
        match rejection {
            Some(_) => Err(&"asr_network"),
            None => Ok(TRANSCRIPT),
        },
        "{scheme} with TLS rejection {rejection:?}"
    );
}
