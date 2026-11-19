use super::*;
use std::future::poll_fn;
use tokio::time::timeout;

async fn next(stream: &mut AudioStream) -> Option<Result<Vec<u8>, OsError>> {
    poll_fn(|cx| stream.as_mut().poll_next(cx)).await
}

#[tokio::test]
async fn streams_blocks_and_drains_tail_before_releasing_slot() {
    let audio = AudioInput::default();
    let (tail_tx, tail_rx) = oneshot::channel();
    let mut stream = audio
        .capture_with(Duration::from_secs(1), move |recording| {
            recording.borrow_mut().started();
            recording.borrow().push(&[1, 0])?;
            tail_rx.blocking_recv().unwrap();
            recording.borrow().push(&[2, 0])
        })
        .await
        .unwrap();
    assert_eq!(next(&mut stream).await, Some(Ok(vec![1, 0])));
    assert!(matches!(
        audio.capture(Duration::from_secs(1)).await,
        Err(OsError::Busy)
    ));
    tail_tx.send(()).unwrap();
    assert_eq!(next(&mut stream).await, Some(Ok(vec![2, 0])));
    assert_eq!(next(&mut stream).await, None);
    assert_eq!(audio.slot.available_permits(), 1);
}

#[tokio::test]
async fn overflow_ends_stream_with_one_error_after_accepted_blocks() {
    let audio = AudioInput::default();
    let (overflow_tx, overflow_rx) = oneshot::channel();
    let mut stream = audio
        .capture_with(Duration::from_secs(1), move |recording| {
            recording.borrow_mut().started();
            for _ in 0..QUEUE_BLOCKS {
                recording.borrow().push(&[1, 0])?;
            }
            let result = recording.borrow().push(&[2, 0]);
            overflow_tx.send(()).unwrap();
            result
        })
        .await
        .unwrap();
    overflow_rx.await.unwrap();
    for _ in 0..QUEUE_BLOCKS {
        assert_eq!(next(&mut stream).await, Some(Ok(vec![1, 0])));
    }
    assert_eq!(next(&mut stream).await, Some(Err(OsError::Overflow)));
    assert_eq!(next(&mut stream).await, None);
    assert_eq!(audio.slot.available_permits(), 1);
}

#[tokio::test]
async fn startup_failure_does_not_return_a_stream() {
    let audio = AudioInput::default();
    let start = audio
        .capture_with(Duration::from_secs(1), |_| Err(OsError::Unavailable))
        .await;
    assert!(matches!(start, Err(OsError::Unavailable)));
    assert_eq!(audio.slot.available_permits(), 1);
}

#[tokio::test]
async fn dropping_stream_or_startup_future_requests_cleanup() {
    let audio = AudioInput::default();
    for start in [false, true] {
        let (entered_tx, entered_rx) = oneshot::channel();
        let mut future = Box::pin(
            audio.capture_with(Duration::from_secs(1), move |recording| {
                entered_tx.send(()).unwrap();
                if start {
                    recording.borrow_mut().started();
                }
                while !recording.borrow().cancelled() {
                    std::thread::park_timeout(Duration::from_millis(1));
                }
                Ok(())
            }),
        );
        if start {
            let stream = future.await.unwrap();
            drop(stream);
        } else {
            tokio::select! {
                result = &mut future => panic!("unexpected startup result: {}", result.is_ok()),
                result = entered_rx => result.unwrap(),
            }
            drop(future);
        }
        let _permit = timeout(Duration::from_secs(2), audio.slot.acquire())
            .await
            .unwrap()
            .unwrap();
    }
}

#[tokio::test]
async fn shutdown_releases_device_with_stream_still_held() {
    let audio = AudioInput::default();
    let _stream = audio
        .capture_with(Duration::from_secs(60), |recording| {
            recording.borrow_mut().started();
            while !recording.borrow().cancelled() {
                std::thread::park_timeout(Duration::from_millis(1));
            }
            Err(OsError::Unavailable)
        })
        .await
        .unwrap();
    timeout(Duration::from_secs(2), audio.shutdown())
        .await
        .unwrap();
    assert!(matches!(
        audio.capture(Duration::from_secs(1)).await,
        Err(OsError::Unavailable)
    ));
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
            .chunks_exact(2)
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
