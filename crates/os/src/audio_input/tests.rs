use super::*;
use std::future::poll_fn;
use tokio::time::timeout;

async fn next(stream: &mut AudioStream) -> Option<Result<Vec<u8>, OsError>> {
    poll_fn(|cx| stream.as_mut().poll_next(cx)).await
}

#[cfg(target_os = "linux")]
#[tokio::test]
#[ignore = "requires an isolated PipeWire session with a driven virtual default microphone"]
async fn pipewire_virtual_microphone() {
    let audio = AudioInput::default();
    let mut stream = audio.capture(Duration::from_secs(1)).await.unwrap();
    let started = tokio::time::Instant::now();
    let mut samples = Vec::new();
    let first = timeout(Duration::from_millis(900), next(&mut stream))
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    assert!(!first.is_empty());
    assert_eq!(first.len() % 2, 0);
    samples.extend(first);
    timeout(Duration::from_secs(2), async {
        while let Some(chunk) = next(&mut stream).await {
            let chunk = chunk.unwrap();
            assert_eq!(chunk.len() % 2, 0);
            samples.extend(chunk);
        }
    })
    .await
    .unwrap();
    assert!(started.elapsed() >= Duration::from_millis(950));
    assert!(
        (28_800..=34_000).contains(&samples.len()),
        "{} bytes",
        samples.len()
    );
    assert!(
        samples
            .chunks(2)
            .any(|sample| i16::from_le_bytes([sample[0], sample[1]]).unsigned_abs() > 100)
    );
    assert_eq!(audio.slot.available_permits(), 1);
}

#[cfg(target_os = "linux")]
#[tokio::test]
#[ignore = "requires an isolated PipeWire session and DESKWAIFU_TEST_SOURCE_MODULE to unload"]
async fn pipewire_virtual_microphone_disconnect() {
    let module = std::env::var("DESKWAIFU_TEST_SOURCE_MODULE").unwrap();
    let audio = AudioInput::default();
    let mut stream = audio.capture(Duration::from_secs(5)).await.unwrap();
    next(&mut stream).await.unwrap().unwrap();
    assert!(
        tokio::process::Command::new("pactl")
            .args(["unload-module", &module])
            .status()
            .await
            .unwrap()
            .success()
    );
    timeout(Duration::from_secs(2), async {
        loop {
            match next(&mut stream).await {
                Some(Err(OsError::Backend)) => break,
                Some(Ok(_)) => {}
                _ => panic!("expected disconnect error"),
            }
        }
    })
    .await
    .unwrap();
    assert_eq!(next(&mut stream).await, None);
    assert_eq!(audio.slot.available_permits(), 1);
}
