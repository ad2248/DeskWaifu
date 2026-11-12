# Rust portal screenshot adapter

Linux/Tokio crate included in the root Cargo workspace; not yet wired into the
application, CMake, UI, OCR, or production capture path. Implements the interactive
XDG Desktop Portal Screenshot protocol with an async zbus session-bus client.

```rust,no_run
use deskwaifu_screenshot::{Client, DEFAULT_TIMEOUT};
# async fn example() -> Result<(), deskwaifu_screenshot::Error> {
let client = Client::session().await?; // connects only; never requests a screenshot
let request = client.capture("", DEFAULT_TIMEOUT)?;
// request.cancel() can explicitly cancel; dropping request also cancels.
let screenshot = request.result().await?;
// Caller must decode and validate screenshot.bytes before treating it as an image.
# Ok(())
# }
```

## Lifecycle and privacy

- Requires a running Tokio runtime. Cloned clients share a single active slot;
  `Busy` rejects overlapping work without queuing or replacing the active request.
- Only explicit `capture` calls invoke Screenshot. Always supplies `interactive=true`,
  `modal=true`, and a process/counter `handle_token`. Parent identifiers must be
  portal-exported identifiers (e.g. `wayland:...`), not window pointers/integers.
- Installs a sender/interface/member/request-namespace signal match **before**
  Screenshot. Signals on either predicted or alternate request handles can arrive
  before the method reply; only the returned path is consumed. Duplicate/stale
  responses do not produce a second result. The signal queue is bounded to 64.
- A single deadline (default 60 seconds) covers subscription, method reply,
  Response, URI validation, metadata and file reading. Connection setup has a
  separate explicit timeout. No automatic retry or non-portal capture fallback.
- Explicit cancellation, dropped handles/futures, timeout and other unsuccessful
  completion attempt `Request.Close` if Response has not already completed.
  A method already in flight is retained for at most two seconds beyond the
  original deadline so a late alternate handle can also be closed. Individual
  best-effort Close calls time out after two seconds; these cleanup tasks do not
  delay the caller's timeout/cancel result. Runtime/bus shutdown may prevent cleanup.
- Errors are typed, fixed strings; raw server errors, credentials, paths and URIs
  are not included. The adapter does not log screenshots or requests.
- No continuous frames, silent capture, region selection API, or claims that image
  pixels correspond to desktop logical coordinates, origins, screen/DPI mappings.
  Portal UI and selection behavior depend on the installed backend.

## File and decoder boundary

Accepts only absolute local `file:` URIs with **empty authority** (even `localhost`
is rejected), no query or fragment, valid percent escapes, and no raw control,
whitespace, backslash or NUL path bytes. No HTTP or remote-host reads. Metadata
and opened-descriptor checks require regular files no larger than **64 MiB**;
a bounded read of at most 64 MiB + 1 byte detects growth after metadata checks.
Nonblocking open prevents a substituted FIFO from hanging the file worker.
Filesystem operations use Tokio's filesystem workers, not synchronous event-loop
reads. A cancelled OS filesystem operation may finish in the background; no
image decoder work is spawned or accumulated by this adapter.

The result is encoded file bytes plus the validated URI, **not a validated image**.
The future caller/decoder must enforce format validity, animation policy,
dimensions (the existing C++ implementation uses 16384 per side), total decoded
pixels (32 Mi), allocation limits, and a decoding deadline before publishing an
image. A byte cap alone does not prevent decompression bombs. Do not infer image
validity from this crate succeeding. The portal-owned file is never deleted,
modified, or moved; ownership of any copied bytes belongs to the caller.

## Offline verification

```sh
cargo fmt --manifest-path rust/screenshot/Cargo.toml -- --check
cargo test --offline --manifest-path rust/screenshot/Cargo.toml
cargo clippy --offline --manifest-path rust/screenshot/Cargo.toml --all-targets -- -D warnings
```

Tests start their own `dbus-daemon --session --nofork` and connect to its explicit
address, never the user's current session bus or live desktop UI. They register a
mock portal and exercise genuine zbus method calls/signals, pre-reply responses,
alternate handles, duplicate signals, option/parent propagation, cancellation,
timeout, drop, late-handle Close, typed sanitized failures, URI rejection, missing
and oversized files, and preservation of portal files. Synthetic non-image bytes
also make the decoder boundary explicit. `dbus-daemon` must be installed; tests
fail rather than silently skip mock D-Bus verification. Real desktop authorization
and backend compatibility have not been manually tested.
