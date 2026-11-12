# overlay

A Wayland `wlr-layer-shell` overlay surface with an EGL window surface, for the
DeskWaifu model window.

## Scope

This crate is a window/input-region primitive with EGL. It owns:

- one `wl_surface` with the `zwlr_layer_surface_v1` role (layer, anchors, size,
  exclusive zone, keyboard interactivity, optional margins, optional named
  output)
- the matching EGL display/config/context/window surface and its
  `wl_egl_window`, plus `make_current`, `swap_buffers` and `size`
- the input region: `Overlay::set_input_region` replaces it with a fresh
  `wl_region` of clipped rectangles and commits, mirroring the C++
  `DesktopWindow::SetInputRegion`. An empty slice is a `wl_region` with no
  boxes, i.e. the whole surface is click-through.

It deliberately does **not** include:

- animation, Cubism, or model code
- any coordinate mapping beyond pixel surface coordinates: callers translate
  model regions into `Rect` themselves
- an event-loop abstraction, settings, or a render loop

EGL is configured with `EGL_ALPHA_SIZE >= 8`, so the caller can clear with alpha
0 and get a fully transparent surface, then draw opaque pixels where the model
is. `Overlay::display()` and `Overlay::egl()` expose the EGL handles for
`eglGetProcAddress`-based GL loading.

Everything is `!Send`/`!Sync`. Each `Overlay` owns its `Connection`; a second
overlay needs a second connection.

## Platform

Linux/Wayland only, and only on compositors that implement
`zwlr_layer_shell_v1` (KWin, Sway). Globals are bound over defensive version
ranges, so a compositor advertising a newer version than the shipped XML is
fine.

This crate is not yet wired into `deskwaifu-rust`, CMake, or the application.
The real desktop authorization and click-through acceptance still belongs to the
C++ smoke suite until the runtime adopts this crate.

## Running the live test

The real compositor proof is opt-in; the ordinary `cargo test` run skips it and
stays green:

```sh
DESKWAIFU_OVERLAY_LIVE_TEST=1 cargo test --test kwin -- --test-threads=1
```

With `DESKWAIFU_OVERLAY_LIVE_TEST=1` the test spawns a private
`kwin_wayland --virtual` instance with its own mode-0700 `XDG_RUNTIME_DIR`, and
requires `kwin_wayland` on `PATH`. It proves:

- the layer surface reaches a `configure` with a nonzero size, with
  `Top | Left` anchors, overlay layer, exclusive zone `-1` and no keyboard
  interactivity
- the EGL context and window surface work: a transparent clear reads back alpha
  0, an opaque clear reads back alpha 255, with no GL error
- real click routing: `org_kde_kwin_fake_input` injects pointer clicks, and a
  full-surface background layer counts the presses that reach it. An empty input
  region sends the click below, a shaped region around the centre sends it to
  the overlay instead, and clearing the region restores click-through.
- a second `Overlay` on its own connection also works

CI does not run this yet; the C++ smoke suite still carries the production
desktop acceptance.
