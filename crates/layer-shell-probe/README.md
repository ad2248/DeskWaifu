# P0 Wayland layer-shell 探针

探针使用 SCTK 在左上角创建 320×240 逻辑像素的 overlay。`wl_shm` 帧以预乘 ARGB8888 绘制两块半透明矩形，输入区域由同一组逻辑矩形生成。`--empty-input` 保留画面并提交显式空输入区域。默认不请求键盘焦点。`--underlay` 启动一个独立的全屏 xdg-shell 下层窗口，绘制棋盘并记录收到的输入。

```sh
cargo build --locked -p layer-shell-probe
target/debug/layer-shell-probe --help
```

## 显示真实 Live2D 模型

参照 [Cubism 构建说明](<../cubism-bridge/README.md>) 设置 SDK 和 GLEW 的路径。在支持 layer-shell 的 Wayland 会话中运行以下命令。

```sh
cargo run --locked -p layer-shell-probe --features native -- \
  --model "$CUBISM_SDK_DIR/Samples/Resources/Haru/Haru.model3.json" --motion Idle:1
```

探针以 640×480 离屏渲染，通过 CPU 回读缩放到 `wl_shm` 缓冲，并从同一帧的 alpha 得到逻辑输入区域。省略 `--motion` 时显示初始帧。此前使用 Mark 在私有 Sway 中验证时，两张连续合成截图有像素差异。模型中央 `(160,112)` 的点击由覆盖层接收，透明角 `(20,20)` 的点击由下层窗口接收。覆盖层隐藏后，模型中央的点击也由下层窗口接收。

This content uses sample data owned and copyrighted by Live2D Inc.

## 验证透明、穿透和隐藏

在私有合成器内运行以下命令。将 `OUTPUT` 设为该会话中的输出名，例如 `HEADLESS-1` 或 `Virtual-0`。

```sh
target/debug/layer-shell-probe --underlay --output "$OUTPUT" > underlay.log &
target/debug/layer-shell-probe --output "$OUTPUT" \
  --hide-after-ms 15000 --exit-after-ms 30000 > overlay.log
```

以输出左上角为原点。点击逻辑坐标 `(150,100)` 时，overlay 应记录 `pointer-press`。按键和点击透明角 `(20,20)` 时，下层窗口应分别记录 `keyboard-key` 和 `pointer-press`。等到 `hidden` 后，再点击 `(150,100)` 应由下层窗口记录。另用 `--empty-input` 启动 overlay，确认可见的 `(150,100)` 也穿透。截图可对照透明角和覆盖区的颜色。

## 验证缩放和两输出重建

将两个输出设为 1×、1.5×。在 Sway 中可用 headless 后端加 `WLR_HEADLESS_OUTPUTS=2`，再分别设置 `output HEADLESS-1 scale 1` 和 `output HEADLESS-2 scale 1.5`。私有 KWin 可用 `kwin_wayland --virtual --output-count 2`，再从该会话执行 `kscreen-doctor output.Virtual-1.scale.1.5`。

```sh
target/debug/layer-shell-probe --output "$OUTPUT" --switch-output "$SECOND_OUTPUT" \
  --switch-after-ms 2000 --exit-after-ms 4000
```

两代 surface 应各有 `configure`、`committed` 和 `frame`，逻辑尺寸都是 `320x240`。若合成器发送 1.5× 的 `preferred_scale`，日志会显示 `numerator=180` 和 `buffer=480x360`。在同一输出使用 `--integer-scale` 可单独验证整数路径。把 `WAYLAND_DEBUG=1` 加到命令前，可确认初次空提交、`ack_configure`、视口和输入 region 与对应 buffer 的提交顺序。

## 私有会话实测记录

- **Sway 1.10.1，headless + Pixman**。`HEADLESS-1` 到 `HEADLESS-2` 切换时，新建第二代 surface，逻辑尺寸保持 `320x240`，`preferred_scale` 由 `120` 变为 `180`，缓冲由 `320x240` 变为 `480x360`。两个输出都返回了帧回调。在另一轮测试中，运行 `swaymsg output HEADLESS-1 disable` 后收到 `output-removed HEADLESS-1`，探针自动在 `HEADLESS-2` 重建并收到帧回调。在 1.5× 输出上强制整数路径时，收到整数缩放 2，并提交 `640x480` 缓冲及空输入区域。
- **KWin 6.3.6，virtual + 软件渲染**。`kscreen-doctor` 将 `Virtual-1` 设置为 1.5×，并报告物理模式 `1200x900`、逻辑几何 `800x600`。从 `Virtual-0` 切换后，第二代 surface 收到 `preferred_scale(180)`，提交 `480x360` 缓冲，逻辑尺寸仍为 `320x240`。两代 surface 都返回了帧回调。
- **Sway 和 KWin，各自嵌套于私有 Xvfb**。从 X11 发送鼠标和键盘事件。overlay 只收到模型内 `(150,100)` 的点击。下层窗口收到 `a` 的按下和释放、透明角 `(20,20)` 的点击，以及隐藏后 `(150,100)` 的点击。两者的合成截图在透明角都是 `RGB(48,64,88)`，在模型内都是 `RGB(56,180,126)`。Sway 隐藏后模型内同点恢复为下层棋盘的 `RGB(80,96,120)`。在 Sway 使用 `--empty-input` 时，该点仍显示合成色 `RGB(56,180,126)`，点击则由下层窗口接收。
