# terminal 插件

`terminal` 负责 Live2D 模型展示、交互和设置。当前验证路径使用 SCTK layer-shell 和 `wl_shm`。请按 [Cubism 构建说明](<../../crates/cubism-bridge/README.md>) 设置 SDK 和 GLEW 路径，然后构建原生插件。

```sh
cargo build --locked -p deskwaifu-terminal --features native
```

将 `libdeskwaifu_terminal.so` 部署为宿主的 `plugins/terminal.so` 后，可用现有 CLI 操作。

```sh
deskwaifucli terminal catalog ./models
deskwaifucli terminal status
deskwaifucli terminal load "$CUBISM_SDK_DIR/Samples/Resources/Mark/Mark.model3.json"
deskwaifucli terminal motions
deskwaifucli terminal motion Idle 1
deskwaifucli terminal expressions
deskwaifucli terminal expression NAME
deskwaifucli terminal outputs
deskwaifucli terminal output HEADLESS-2
deskwaifucli terminal hide
deskwaifucli terminal show
```

`terminal catalog [DIRECTORY]` 无需 Cubism SDK 或 Wayland。省略 `DIRECTORY` 时，优先使用非空的 `DESKWAIFU_MODEL_DIR`，否则使用 `$XDG_DATA_HOME/deskwaifu/models`。如果 `XDG_DATA_HOME` 未设置或不是绝对路径，则使用 `$HOME/.local/share/deskwaifu/models`。XDG 默认目录尚未创建时返回 `{"models":[]}`，显式指定或由 `DESKWAIFU_MODEL_DIR` 指定的目录不存在时返回错误。

模型默认目录也可以在宿主的共享配置中设定。`terminal catalog DIRECTORY` 的显式参数优先于该配置，随后才读取环境变量与 XDG 默认值。

```jsonc
{
  // ~/.config/deskwaifu/config.jsonc，权限 0600
  "terminal": {
    "model_directory": "/home/user/Live2D/models",
    "startup_model": "/home/user/Live2D/models/Haru/Haru.model3.json",
    "mouse_look_radius": 0.3,
    "output": "DP-1",
    "emotion_timeout": 4.0,
  },
  "voice_input": {
    "max_recording_seconds": 15,
  },
}
```

目录扫描返回 `{"models":["/absolute/path/Model.model3.json"]}`。结果是按路径排序的 UTF-8 绝对路径，可传给 `terminal load`。扫描只匹配以 `.model3.json` 结尾的普通文件，不读取模型内容，也不跟随根目录内的符号链接。从根目录（深度 0）最多下探 8 层，共检查最多 10,000 个目录项。超过目录项上限或转义后超过 64 KiB 的 IPC 响应上限时返回错误。

`terminal motions` 和 `terminal expressions` 返回与 `terminal status` 相同的 JSON 快照。`snapshot.motions` 列出已加载模型的动作组，`snapshot.expressions` 列出可供 `terminal expression NAME` 使用的名称。未加载模型或后端不可用时，这两个数组为空。

`terminal outputs` 从 `snapshot.outputs` 列出可用输出。`terminal output NAME` 等待目标表面配置完成，然后保存连接器和目标屏上的居中位置，清除裁剪并保留缩放。省略名称时恢复合成器自动选择。`snapshot.output` 是覆盖层当前所在的输出，`snapshot.selected_output` 是明确选择的输出。写入失败或名称无效时保留原有覆盖层。

`terminal focused-output` 查询 Sway 或 Hyprland 当前焦点屏并临时移动覆盖层。查询失败时可用 `terminal output NAME` 指定输出。滚轮缩放停止约半秒后保存变换。若保存失败，面板会显示错误，可执行 `terminal save-transform` 重试。

可用 `terminal opacity VALUE`（`0..=1`）、`terminal hover-opacity [VALUE]`、`terminal crop [X Y W H]` 和 `terminal transform X Y Z [CX CY CW CH]` 本地预览效果。透明度为零时，模型原轮廓仍接收输入。裁剪使用输出左上角为原点的逻辑坐标，裁剪区域外的画面与输入同时移除。`terminal lip-sync [VALUE]` 设置 `0..=1` 的嘴部开度，`terminal look [X Y]` 设置各轴为 `-1..=1` 的视线目标。省略参数会清除对应覆盖。

若模型旁存在同名 `.vtube.json`，面板会列出其中的快捷键、动作、激活状态和不支持原因。支持面板窗口内的 `ToggleExpression`，按钮与快捷键共用同一开关。换模清除持久表情，普通表情按 `emotion_timeout` 秒恢复，`-1` 关闭恢复计时。

`terminal gpu` 查看当前渲染设备与下次启动配置。`terminal gpu 0000:04:00.0`、`terminal gpu /dev/dri/renderD128` 或 `terminal gpu auto` 保存下次启动的选择。指定设备不可用时，宿主仍可查询状态和重新选择设备。

启用 KWin 全局视线需要构建时使用 `./scripts/build.sh --kwin-cursor`，并安装 `libsystemd` 开发库。将生成的 `deskwaifu-cursor-guard` 放在宿主旁，设置 `mouse_look_radius` 后可在 KDE 会话中启用。样本不可用时使用覆盖层局部指针。

左键短按触发模型的点击动作，中键循环皮肤，右键循环模型。左键拖动进入屏幕编辑，顶部手柄同时移动模型与裁剪框，边角调整裁剪范围。按 Esc 取消，双击框边或点框外应用。保存失败时草稿保留，可再次操作重试。

面板使用同一 IPC 连接执行 `terminal edit begin`、`terminal edit update X Y Z [CX CY CW CH]`，再选择 `terminal edit apply` 或 `terminal edit cancel`。应用将位置、缩放、裁剪写入 `terminal.transform`。取消或连接断开恢复编辑前的状态，保存失败保留当前草稿。每个 CLI 调用独立连接，适合单次预览。

投递成功时返回 `accepted_operation_id`。使用 `terminal status` 查询 `snapshot.pending` 和最近 32 条 `snapshot.completed`，其中 `error` 为 `null` 表示操作成功。`snapshot.visible` 表示覆盖层已经提交可见帧。切换加载失败时保留旧模型和会话代号。切换成功后，排队中属于旧模型的动作和表情会返回会话已变更。脚本可在加载完成后再查询并提交新模型的动作。默认构建无需 SDK，渲染状态返回 `unavailable`。
