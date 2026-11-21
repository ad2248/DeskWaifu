# Live2D 桌宠设计

## 组件与线程

`terminal` 是 Linux x86_64 / Wayland 上负责 Live2D 模型展示与交互的动态插件。宿主线程通过 `Plugin::on_command` 接收命令，读取最近的状态快照，管理编辑及配置写入，并将渲染操作投递到有界队列。`layer-shell-probe` 提供呈现 worker，同一专用线程持有 Wayland 连接、surface、EGL 上下文和 Cubism 模型。终止通过独立于命令队列的通道唤醒 worker，插件等线程退出后再返回 `stop`。 [插件入口](<../plugins/terminal/src/lib.rs#L805>) · [呈现 worker](<../crates/layer-shell-probe/src/worker.rs#L208>)

`cubism-bridge` 通过 C ABI 调用 Cubism Native SDK。C++ 桥接层管理 Framework、模型、纹理、动画、表情和 OpenGL 渲染器。Rust 包装将原生句柄限制在所属线程，使用离屏 EGL 上下文和 OpenGL 渲染 `RGBA8` 像素。默认 workspace 构建启用 `terminal` 的原生渲染和 KWin 全局指针支持，设置 `CUBISM_SDK_DIR` 和 `CUBISM_GLEW_DIR` 后由 Cargo 通过 `build.rs` 调用 CMake 构建桥接。单独构建 `deskwaifu-terminal --no-default-features` 时无需 SDK，模型目录查询仍可用，渲染状态为 `unavailable`。 [构建与设备接口](<../crates/cubism-bridge/README.md>) · [插件 features](<../plugins/terminal/Cargo.toml#L10>)

## 呈现与输入

worker 用 SCTK 建立 `overlay` 层的 layer-shell surface，设置 `exclusive_zone=-1`，初次空提交后等待 configure。SCTK 将离屏渲染经 CPU 读回的像素写入 `wl_shm` 缓冲区，随输入区一起提交。分数缩放使用 `fractional-scale` 和 `viewporter`，所需协议缺失时使用整数缩放。合成器的帧回调控制提交节奏。 [surface 创建](<../crates/layer-shell-probe/src/probe.rs#L1363>) · [帧提交](<../crates/layer-shell-probe/src/probe.rs#L1784>)

普通模式从模型 alpha 提取逻辑坐标中的命中矩形，并按裁剪区域裁切。显示透明度在命中区域计算之后应用，透明度为零时模型原轮廓仍接收输入。屏幕编辑启用键盘交互和整屏输入，用于拖动、裁剪及取消。隐藏状态停止提交模型画面及模型输入。 [绘制和输入计算](<../crates/layer-shell-probe/src/probe.rs#L1725>)

| 能力 | 接口及约束 |
| --- | --- |
| 输出选择 | `terminal outputs` 查询 Wayland 输出，`terminal output [NAME]` 保存选定连接器或恢复自动选择。`terminal focused-output` 通过 Sway 或 Hyprland IPC 临时移至焦点屏。 |
| 跨屏拖动 | Hyprland 光标查询为屏幕编辑提供跨屏位置。查询不可用时可在当前输出拖动。 |
| 全局视线 | KDE 会话可使用 KWin D-Bus 临时脚本采样全局指针，缺少可用样本时使用覆盖层局部指针。插件退出时卸载脚本。脚本运行后会检测宿主消失并卸载自身，强制终止发生在加载后、运行前时可能残留到 KWin 会话结束。 |
| 渲染设备 | EGL 自动选择设备或根据用户配置的完整 PCI 地址、绝对 DRM render node 路径创建离屏上下文。设备设置在下一次启动生效。初始化失败时宿主仍可查询状态并改选设备。 |

## 命令与控制面板

`terminal status` 读取最新快照。投递成功的操作返回 `accepted_operation_id`，可通过 `snapshot.pending` 和最近 32 条 `snapshot.completed` 查询结果。新模型加载成功后替换在用模型并更新会话代号，针对旧会话的动作和表情不生效。加载失败保留在用模型。后端失败在状态中上报，宿主可继续处理命令。 [操作状态接口](<../plugins/terminal/README.md>)

`deskwaifu-terminal-panel` 是独立的 GPUI Kit 进程。面板通过 `panel-tabs` 获取已注册标签页，再使用同一 JSON socket 协议查询模型、发起动作、显示结果。屏幕编辑由覆盖层处理。面板编辑通过同一 IPC 连接持有 `terminal edit` 事务，应用时保存位置、缩放和裁剪，取消或断开后回滚。保存失败保留草稿。 [面板的 IPC 类型](<../apps/terminal-panel/src/ipc.rs#L13>) · [宿主架构](<程序架构.md>)

## 配置导入

[配置导入脚本](<../scripts/migrate-live2d-config.py>) 接受归档桌面插件的 UTF-8 配置文件，向 stdout 输出含 `terminal` 节的 JSON，向 stderr 输出诊断。输入文件仅供读取。将输出中需要的字段手动合并到宿主共享 JSONC 的 `terminal` 节。

```sh
python3 scripts/migrate-live2d-config.py LEGACY_CONFIG \
  --model-directory /absolute/model/catalog \
  --startup-model /absolute/model/catalog/Haru/Haru.model3.json
```

输出可包含 `output`、`gpu`、透明度、注视半径、表情超时、裁剪、模型目录和启动模型。负数表情超时统一转换为 `-1`。位置和缩放需在面板预览后手动设置，合并裁剪时可保留已有 `transform` 的位置和缩放。目录及启动模型选项接收绝对路径，启动模型应为 `.model3.json`。脚本会报告不支持或无法自动映射的字段。

## 待验收

双硬件 GPU 的显式设备选择待使用两个不同的 PCI 地址验证。 [设备选择测试](<../crates/cubism-bridge/README.md>)
