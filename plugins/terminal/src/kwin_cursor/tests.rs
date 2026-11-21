use super::*;

fn sample(now: Instant) -> Sample {
    Sample {
        observed: now,
        cursor: [-960.0, 900.0],
        origin: [-1920.0, 0.0],
        size: [1920.0, 1080.0],
        fullscreen: true,
    }
}

#[test]
fn maps_logical_geometry_with_negative_origin_without_clamping() {
    let now = Instant::now();
    let mut sample = sample(now);
    assert_eq!(sample.local(960, 540), Some((480.0, 450.0)));
    sample.cursor = [-3000.0, 1200.0];
    assert_eq!(sample.local(960, 540), Some((-540.0, 600.0)));
    assert_eq!(sample.local(0, 540), None);
    sample.size[0] = 0.0;
    assert_eq!(sample.local(960, 540), None);
}

#[test]
fn invalidates_samples_at_ttl_and_when_an_output_generation_changes() {
    let now = Instant::now();
    let mut target = Target {
        output: "DP-2".into(),
        generation: 3,
        sample: None,
    };
    target.put(sample(now), "DP-1", "3");
    target.put(sample(now), "DP-2", "1");
    assert!(target.fresh(now).is_none());
    target.put(sample(now), "DP-2", "3");
    assert!(
        target
            .fresh(now + STALE - Duration::from_millis(1))
            .is_some()
    );
    assert!(target.fresh(now + STALE).is_none());
    // A connector disappearing invalidates its position before the TTL elapses.
    target.put(sample(now), "", "3");
    assert!(target.fresh(now).is_none());
}

#[test]
fn queued_stale_sample_cannot_become_fresh_on_receipt() {
    let now = UNIX_EPOCH + Duration::from_secs(2);
    assert_eq!(sample_age(now, 1500), Some(Duration::from_millis(500)));
    assert_eq!(sample_age(now, 1000), None);
    assert_eq!(sample_age(now, 2001), None);
    assert_eq!(number("NaN"), None);
    assert_eq!(number("Inf"), None);
    assert_eq!(number("1,25"), None);
    assert_eq!(number("1.25"), Some(1.25));
}
