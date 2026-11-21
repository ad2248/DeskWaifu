# Cubism / EGL 原型

使用本地 Cubism Native SDK 5-r.5 与生成过的 GLEW 源码（已验证 GLEW 2.3.1）。Linux x86_64 构建需要 CMake、C/C++ 编译器及 EGL、OpenGL 开发库。默认构建不启用原生功能，无需 SDK 或图形库。

```sh
export CUBISM_SDK_DIR=/path/to/CubismSdkForNative-5-r.5
export CUBISM_GLEW_DIR=/path/to/glew
mkdir -p target/live2d

env -u DISPLAY -u WAYLAND_DISPLAY LIBGL_ALWAYS_SOFTWARE=1 \
  cargo run --locked -p cubism-bridge --features native --example offscreen -- \
  "$CUBISM_SDK_DIR/Samples/Resources/Mark/Mark.model3.json" \
  target/live2d/mark.png Idle 1 40

env -u DISPLAY -u WAYLAND_DISPLAY LIBGL_ALWAYS_SOFTWARE=1 \
  cargo test --locked -p cubism-bridge --features native --test offscreen -- --ignored
```

示例把第 40 帧保存为 640×640 RGBA PNG。省略动作参数时输出初始帧。`Model::render` 输出自上而下的预乘 RGBA8，PNG 导出时还原为直通 alpha。GLSL 在构建时从 SDK 嵌入，运行时只读取所选模型及其资源。使用和分发 SDK 与样例前请核对其适用条款。This content uses sample data owned and copyrighted by Live2D Inc.

离屏路径使用 EGL 与 OpenGL compatibility profile，将一套运行时绑定在调用线程上。它用于可信模型的离屏验证。

`Runtime::new` 使用 EGL 自动选择。`Runtime::devices` 枚举 EGL 公布的设备，`Runtime::new_on_device` 按绝对 DRM render node 路径（支持 `/dev/dri/by-path` 符号链接）或完整 PCI 地址 `dddd:bb:ss.f` 创建离屏运行时。显式选择失败会返回错误。用 `Runtime::active_device` 读取驱动报告的活动设备，用 `Runtime::renderer` 查看 GL renderer。软件设备可能没有 DRM 路径或 PCI 地址。

在支持设备枚举的机器上，可将 `CUBISM_TEST_PCI` 设为本机显卡地址，运行原生 `explicit_device_selection` 测试。测试按 PCI 地址和 render node 创建运行时、校验活动设备并渲染样例。设置 `CUBISM_TEST_RENDERER` 可断言 renderer 包含指定字符串。测试硬件渲染时取消 `LIBGL_ALWAYS_SOFTWARE`。

双显卡验收时同时设置 `CUBISM_TEST_SECOND_PCI`，如需校验第二个 renderer 字符串则设置 `CUBISM_TEST_SECOND_RENDERER`。测试依次创建和释放两块卡上的 Cubism/EGL 运行时，并在每次初始化后核对活动设备及样例像素。

```sh
env -u LIBGL_ALWAYS_SOFTWARE CUBISM_TEST_PCI="$FIRST_PCI" CUBISM_TEST_SECOND_PCI="$SECOND_PCI" \
  cargo test --locked -p cubism-bridge --features native --test offscreen \
  explicit_device_selection -- --ignored --nocapture
```

## P1 Wayland 呈现对照

`window_bench` 在同一个固定 1× layer-shell 客户端中切换两条呈现路径。`egl` 在同一 `wl_display` 上创建带 alpha 的 window surface，将模型 FBO blit 到 EGL back buffer，并从每轴缩小约 8 倍的 FBO 读取轮廓采样。`shm` 使用离屏 EGL 完整读回，再转换为预乘 `WL_SHM_FORMAT_ARGB8888` 写入已释放的缓冲区。两条路径都在同一次 commit 应用对应帧的输入区域，并使用 compositor frame callback 节流。

`wayland-window` 示例额外需要 wayland-client、wayland-egl、wayland-protocols、wayland-scanner、pkg-config，以及 `wlr-layer-shell-unstable-v1.xml`。如未安装 wlr-protocols，可将 `CUBISM_LAYER_SHELL_XML` 指向本机 Cargo 缓存中 `wayland-protocols-wlr` 自带的 XML。桥接 API 可由启用 `native` 的调用方使用，调用方负责创建和维护 Wayland 连接及 native window。

```sh
export CUBISM_LAYER_SHELL_XML=/path/to/wlr-layer-shell-unstable-v1.xml
cargo build --locked --release -p cubism-bridge --features wayland-window --example window_bench
model="$CUBISM_SDK_DIR/Samples/Resources/Mark/Mark.model3.json"
bench=target/release/examples/window_bench
# 通过私有 Wayland 会话的 XDG_RUNTIME_DIR 和 WAYLAND_DISPLAY 运行。
export LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=4
"$bench" egl "$model" 640 30 target/live2d/mark-egl.png
"$bench" shm "$model" 640 30 target/live2d/mark-shm.png
cmp target/live2d/mark-egl.png target/live2d/mark-shm.png
TIMEFORMAT='cpu_percent=%P user_s=%U sys_s=%S total_s=%R'
for size in 640 1600; do
  for path in egl shm shm egl egl shm; do
    time "$bench" "$path" "$model" "$size" 150
  done
done
```

可选 PNG 验证 EGL back buffer 的真实像素与离屏输出。写图和 20 帧热身不计入帧时间和 IPC 采样。`frame_work` 计量绘制、回读、输入区域和提交，`present` 是其中的输入区域加 swap 或 SHM 转换加 commit，`frame_interval` 含 compositor 回调等待。独立线程每次收到回复后等待 2ms，再用本地 Unix datagram 发下一次请求，由呈现线程在帧间回应。CPU 采用 Bash `time`，包含初始化和热身。`readback_bytes_per_frame` 仅统计应用请求从 GL 读回的字节，驱动内部传输另计。

### 私有 Sway 测量

Sway 1.10.1 headless + pixman，Mesa llvmpipe（LLVM 19.1.7），Cubism Mark `Idle` 组索引 1。每组选取三次各 150 帧的中位数，帧及 IPC 的 p95 先在单次运行内计算。

| 尺寸 | 路径 | 帧处理 p95 | 帧间隔 p50 | Worker IPC RTT p95 | 应用 CPU | GL 回读字节/帧 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 640² | EGL | 10.125 ms | 17.065 ms | 8.538 ms | 151.5% | 25,600 |
| 640² | `wl_shm` | 8.633 ms | 17.065 ms | 6.429 ms | 118.6% | 1,638,400 |
| 1600² | EGL | 37.497 ms | 39.501 ms | 36.490 ms | 264.8% | 160,000 |
| 1600² | `wl_shm` | 28.859 ms | 34.294 ms | 28.012 ms | 221.5% | 10,240,000 |

十二次运行均无 IPC 超时。640² 两个首帧 PNG 逐字节一致，包含 369,102 个全透明、5,218 个半透明、35,280 个全不透明像素。`grim` 捕获两个实际合成结果，模型区域改变了约 40,400 个像素，透明角落保留背景色。Wayland trace 显示同一个 surface 的 `set_input_region`、EGL attach 和 `commit` 顺序相邻，也显示软件 EGL 自身通过 Wayland `wl_shm` 分配交换缓冲。

软件路径下，EGL 将应用显式回读量缩小到 1/64。软件 EGL 的交换缓冲和同步使帧处理、CPU 占用以及 worker socket 延迟都更高。

### Intel DG1 硬件测量

同一 Sway 版本、客户端、模型和重复次数，改用 headless + GLES2，在 `/dev/dri/renderD128` 上启动 Sway，并取消 `LIBGL_ALWAYS_SOFTWARE`。两条客户端路径和合成器均报告 Mesa Intel Iris Xe DG1。EGL 的 Wayland trace 显示 DMA-BUF-backed buffers（`zwp_linux_buffer_params_v1.create_immed`）。

| 尺寸 | 路径 | 帧处理 p95 | 帧间隔 p50 | Worker IPC RTT p95 | 应用 CPU |
| --- | --- | ---: | ---: | ---: | ---: |
| 640² | EGL | 1.789 ms | 16.263 ms | 0.996 ms | 27.6% |
| 640² | `wl_shm` | 3.336 ms | 16.267 ms | 1.426 ms | 31.5% |
| 1600² | EGL | 3.197 ms | 16.254 ms | 1.247 ms | 29.1% |
| 1600² | `wl_shm` | 16.130 ms | 17.771 ms | 14.436 ms | 60.2% |

硬件每帧 GL 回读字节数与软件测试相同。十二次运行均无 IPC 超时。同一设备的两个 640² PNG 逐字节一致，包含 369,096 个全透明、5,295 个半透明、35,209 个全不透明像素。

呈现选择取决于驱动实际使用的交换路径。DG1 支持在 DMA-BUF 可用时推进硬件 EGL 集成，`wl_shm` 可覆盖软件渲染和接入失败。当前原型固定 1× 和窗口尺寸，IPC 测量覆盖呈现 worker 的帧间响应。正式插件集成仍需验证实际宿主 IPC、输出重建与 EGL context 丢失。
