# deskwaifu-ocr-engine

Rust-owned OCR inference engine. It replaces the C++ implementation in
`plugins/perception/native/OcrEngine.cpp` / `.hpp`: the same preprocessing, DB
postprocessing, box extraction, line cropping, recognition resizing and CTC
decoding, driven by the system ONNX Runtime.

This crate is **inference only**.

- **Not included:** screenshot capture, selection UI, clipboard handling, Qt or
  any GUI toolkit, plugin lifecycle, IPC/socket transport, threading policy,
  timeouts, cancellation, model downloading or model directory management.
- **Included:** loading and validating `inference.onnx` + `inference.yml`
  pairs, running detection and recognition, and producing the ordered text
  lines with boxes and confidences that the native module returned.

## Dependencies

Runtime crates (deliberately few):

| crate | why |
| --- | --- |
| `deskwaifu-perception-domain` (`path = "../perception-domain"`) | reuses the already-ported `ctc::decode_ctc` greedy CTC decoder and `line_order::sort_line_boxes` local row sort instead of reimplementing them |
| `image` (`png`, `jpeg`, `bmp`, `pnm`, no default features) | decodes input images to RGB8 |
| `yaml-rust2` | parses `inference.yml`; a real YAML parser is used rather than a hand-written one |
| `tempfile` (dev only) | isolated model directories in tests |

ONNX Runtime is **not** a crate dependency.

## ONNX Runtime requirement

The crate links against the system shared library; nothing is vendored and
nothing is downloaded at build time.

- Needs `/usr/lib/libonnxruntime.so` (or any location the linker already
  searches) and a runtime that provides **C API version 16 or newer**.
- `build.rs` only emits `cargo:rustc-link-lib=onnxruntime`. If your runtime lives
  somewhere unusual, set `DESKWAIFU_ONNXRUNTIME_LIB_DIR` to that directory.
- A missing development library fails the build at link time with an unresolved
  symbol, not with a runtime surprise.
- The `ort` crate was rejected: several of its configurations want to download a
  bundled runtime, which this project forbids. The C API is bound by hand
  instead.

### Binding approach

`src/onnx.rs` declares the leading region of `OrtApi` as a `#[repr(C)]` struct of
function pointers and obtains it through `OrtGetApiBase().GetApi(16)`.
`OrtApi` is append-only, so a prefix is enough as long as every slot is declared
in order; unused slots are pointer-sized placeholders. The slot indices were
checked against the compiler-computed `offsetof` layout of the ONNX Runtime 1.16
and 1.29 headers (e.g. `CreateEnv` = 3, `CreateSession` = 7, `Run` = 9,
`SessionGetInputName` = 36, `CreateTensorWithDataAsOrtValue` = 49,
`ReleaseSession` = 95).

Two ownership rules of the C API matter and are respected:

- `CastTypeInfoToTensorInfo` returns a **borrowed** pointer ("Do not free this
  value, it will be valid until `type_info` is freed"). Freeing it is a
  double-free; this was a real bug caught by the test suite.
- `GetTensorTypeAndShape` returns a **new object** that must be released.

## Behaviour ported from the C++ engine

Every `Require(...)` in `OcrEngine.cpp` has a counterpart that fails with a typed
error instead of proceeding:

| C++ check | here |
| --- | --- |
| metadata readable, ≤ 4 MiB | `metadata::metadata` → `model_missing` / `model_incompatible` |
| `Global.model_name` is `PP-OCRv6_small_det` / `_rec` | `metadata::metadata` |
| `DecodeImage.img_mode == BGR`, `channel_first == false` (both models) | `load_detector` / `load_recognizer` |
| `DetResizeForTest` is null | `load_detector` |
| `NormalizeImage` mean/std/scale/order exactly the reference values | `load_detector` |
| `ToCHWImage` present | `load_detector` |
| `RecResizeImg` (or the historic `ResRecizeImg`) `image_shape == [3,48,320]` | `load_recognizer` |
| `PostProcess.name` is `DBPostProcess` / `CTCLabelDecode` | both loaders |
| `thresh`, `box_thresh` in `(0,1)` and finite; `unclip_ratio` in `(0,10]` and finite; `max_candidates` in `(0,10000]` | `load_detector` |
| dictionary is a non-empty sequence shorter than 100000 | `load_recognizer` |
| every dictionary entry is a scalar, non-empty, without U+FFFD | `load_recognizer` |
| `use_space_char` absent or true | `load_recognizer` |
| `ValidateSession`: one input/output, tensor types, float32, input `[1,3,-1,-1]` (det) or `[1,3,48,-1]` (rec) | `engine::validate_session` |
| detector output `[1,1,rows,cols]` with non-zero sides | `validate_session` |
| recognizer output `[1,steps,classes]` with `classes == dictionary len` | `validate_session` |
| runtime detector output sides in `1..=4096` | `detect::validate_output_shape` |
| constructor self-test on blank 64x64 and 48x320 images | `Engine::load` → `self_test` |

Notes on fidelity:

- `element_type`/`dtype` mismatches are reported as `model_incompatible`, and a
  file ONNX Runtime cannot parse as `model_load_failed`, matching the native
  error codes.
- `yaml-cpp` semantics are reproduced: a real scalar converted to `int`
  truncates (`max_candidates: 3000.9` → `3000`), and booleans accept the YAML 1.1
  spellings (`true`/`yes`/`on`).
- The recognition input width is `min(3200, max(320, height*cols/rows))` with the
  C++ truncation for the tensor width and `ceil` for the resized width.

## Geometry: validated against OpenCV, not re-derived by eye

The C++ relied on OpenCV. Rather than approximate it, the primitives are ported
and diffed against OpenCV's own output:

- `geometry::resize_linear` — `cv::resize` with `INTER_LINEAR`. Uses the 11-bit
  fixed-point coefficient path OpenCV uses for 8-bit images, including the
  per-coefficient rounding (a pair may sum to 2047 or 2049) and the `(b0*(s0>>4)
  ... + 2) >> 2` combination. Also keeps `fy` unclamped and clips row indices,
  as `resizeGeneric_Invoker` does — clamping `fy` instead changes 8-bit
  rounding. **Bit-identical on 139,764 sampled channel values** across 9
  image/scale combinations.
- `geometry::warp_perspective_cubic` — `cv::warpPerspective` with
  `INTER_CUBIC` + `BORDER_REPLICATE`. Reproduces the `1/32`-quantised
  `BicubicTab_i` table at the `1<<15` scale, the `saturate_cast<short>` clamp, and
  the table-sum fix-up (a weight of exactly 1.0 stores as 32767, which is what
  makes the fix-up fire). Match is exact except **2 of 2,220** samples on
  border-clamped maps, where a single level differs; the comparison in the tests
  allows one step for that case and documents it.
- `geometry::min_area_rect` — `cv::minAreaRect` including the Sklansky convex
  hull (with OpenCV's stack-swap order for `clockwise = false`), the rotating
  calipers pass and the `n <= 2` special cases. **All 312** generated shapes
  agree to within 8e-6 (float32 rounding), with no difference above 1e-3.
- `detect::corners` — `Corners(...)`: sort by x, order each pair by y, emit
  top-left, top-right, bottom-right, bottom-left.
- `contours::find_contours` — `cv::findContours(RETR_LIST, CHAIN_APPROX_SIMPLE)`
  for an 8-bit mask: zero-padded border following, direction-change recording,
  newest-contour-first order. **Identical on 84 masks**, including holes,
  diagonal touching and single pixels.
- `geometry::fill_polygon` — used only for the DB box score mask.

## Result type

`Recognition` carries `width`, `height`, ordered `lines` and `elapsed_s`.
Each `Line` has `text`, `confidence` (from `decode_ctc`) and a `polygon` of four
points. `Recognition::to_json` emits the same object the native module returned:

```json
{"width":64,"height":64,"lines":[{"text":"中' ","confidence":1,"polygon":[[0,10],[63,10],[63,49],[0,49]]}],"elapsed_s":0.01}
```

Line ordering comes from `deskwaifu_perception_domain::line_order`, and text and
confidence from `deskwaifu_perception_domain::ctc`.

## Tests

```bash
cargo fmt --check
cargo test
cargo clippy --all-targets -- -D warnings
```

The suite does not need Python, network access or real weights.

`tests/onnx_fixture` encodes tiny constant-graph ONNX models in Rust (IR 8, opset
13, `ModelProto`/`GraphProto`/`Constant`/`TensorProto`), the same recipe as
`plugins/perception/tests/test_ocr_native_models.py`. `tests/inference.rs`
therefore **creates and executes real ONNX Runtime sessions**; the inference
path is exercised, not merely compiled. `a_real_session_executes_the_constant_graph`
exists specifically to prove that.

What the tests establish:

- a session loads, reports its declared shapes, and returns the graph's constant
  output;
- valid models construct an engine, and a detection + recognition pass returns
  `中' ` with confidence `1` and a box whose corners match OpenCV's DB
  postprocessing of the same probability map to within half a pixel;
- blank and uniform images yield no lines; a box under `box_thresh` and a
  component thinner than the 3-pixel side limit are dropped;
- each rejected metadata variant in the table above fails with the right typed
  error, including wrong `img_mode`, `channel_first`, non-null
  `DetResizeForTest`, wrong mean/std/scale/order, wrong `image_shape`, wrong
  postprocess names, out-of-range `thresh`/`box_thresh`/`unclip_ratio`/
  `max_candidates`, missing/empty/blank dictionary entries and
  `use_space_char: false`;
- a recognizer whose declared classes disagree with the dictionary, static
  recognizer dimensions, a non-float input and an oversized runtime detector
  output are rejected at construction;
- an unloadable file reports `model_load_failed`.

### Real models

Real weights are not part of this crate. One test is `#[ignore]`d and runs only
when `DESKWAIFU_OCR_MODEL_DIR` points at a directory containing
`PP-OCRv6_small_det` and `PP-OCRv6_small_rec`:

```bash
DESKWAIFU_OCR_MODEL_DIR="$HOME/.local/share/deskwaifu/ocr/models" \
  cargo test -- --ignored real_models
```

It loads the real models and asserts a blank image produces no lines. It makes
**no accuracy claim**, and no accuracy claim is made anywhere else: the
constants above are synthetic fixtures, not OCR quality evidence.

## Not yet wired up

- `deskwaifu-rust` does not depend on this crate yet.
- CMake still builds the C++ `deskwaifu-ocr` module; nothing here is built or
  installed by it.
- Packaging does not ship this crate, and the AUR/installer flow is unchanged.
- The C++ plugin, its Qt backend interface and the OCR client are untouched.
