# IPC 与外部控制

> 核对基线：`0058b7e`。本页约定传输、命令路由和控制源语义；领域业务验收分别链接对应主题。整理文档未连接正在运行的后端。

## 目标与非目标

通过本机 Unix socket 向桌宠查询状态、执行命令或持续提供口型/视线。默认路径 `$XDG_RUNTIME_DIR/deskwaifu.sock`，回退 `/tmp/deskwaifu.sock`；不提供网络 RPC、远程身份认证或不可信插件沙箱。不应将 socket 暴露到宿主网络/共享可写挂载并假定安全。

## 需求与验收

- **IPC-01 帧格式**：请求和响应均为换行分隔 JSON 对象。请求不能含注释或重复键，只按顶层字段路由。领域命令以 `ok` 表示结果；未知命令、缺失/歧义能力、超时和非法提供者回复明确失败，不回退到平台业务对象。
- **IPC-02 类型**：名称、ID、token 必须为字符串，数值字段不接受数字字符串，整数拒绝小数和溢出。普通布尔使用 JSON boolean；`reset`/`stream_source` 保留历史 `true/false`、`0/1` 及相应字符串形式，不把它扩展到其他字段。
- **IPC-03 限制**：最多 16 个并发连接；请求行不超过 64 KiB，回复不超过 8 MiB（均不含末尾换行）。超限关闭对应连接，满连接时拒绝新连接，不能替换健康客户端。
- **IPC-04 顺序与背压**：每连接按请求顺序回复，最多保留一个待发送回复，期间不读/执行该连接的新请求；慢客户端不得阻止其他连接前进。
- **IPC-05 有界调度**：单次处理最多 4 次 accept 和 16 行请求（每连接最多 4 行），轮换首连接；每连接读最多 4 KiB、写最多 64 KiB 且最多 16 次尝试。空行和中断尝试也占预算。此为传输预算，不是领域命令执行时间的硬实时保证。
- **IPC-06 CLI**：`deskwaifu-ctl` 只需 Python 3 标准库；保留数字外观的字符串名称，按协议类型编码参数。仅收到完整有效且 `ok:true` 的响应退出 0；业务失败、超时（5 秒）、空/截断/损坏响应和通信失败退出 1 并显示错误。
- **IPC-07 结果未知**：丢失响应不证明命令未执行。GUI/脚本应查询有效状态而不是自动重放 toggle、保存等修改。发送前取消可撤回排队命令，不能撤销已经执行的修改。
- **IPC-08 外部控制源**：非流式单次口型/视线写入保留为共享持久 CLI 源，不因该连接断开而清除；`stream_source:true` 使用连接专有身份，必须保持长连接，断开仅释放该连接的控制。
- **IPC-09 仲裁**：口型和视线分别仲裁，存活流式源优先于持久 CLI 源，同级最后更新者优先；源释放后恢复前一个合格源。`reset` 清除对应通道，`release_source` 同时释放两者，流式请求只影响所属连接。
- **IPC-10 模型身份**：真正切换模型清空全部旧外部源；选择失败或仍为当前模型时保留。`get_control_status` 提供当前有效控制值；快捷键等使用查询返回的模型 token，拒绝旧 token。

## 命令分组与权威来源

- 模型目录、动作/表情/皮肤、可见性、快捷键、显示器、渲染设备、透明度/注视半径及变换命令：[模型交互与显示](模型交互与显示.md)。
- 变换事务、保存结果查询、重试及冲突：[配置与保存](配置与保存.md)。
- `get_speech_settings`/`set_speech_settings`：[语音与凭据](语音与凭据.md)。
- `get_ocr_status`、`ocr_recognize`、`get_ocr_result`、`cancel_ocr`、`reading_clip` 及状态：[图像识别与剪贴板朗读](图像识别与剪贴板朗读.md)。
- `quit` 与重启顺序：[产品范围与进程生命周期](产品范围与进程生命周期.md)。

常用命令调用示例仍见[根 README 的 API 参考](../../README.md#ipc-api)。领域参数和错误由对应提供者解释；此处不复制一份易漂移的逐参数手册。

## 现行设计与证据边界

[LAppIPC](../../plugins/ipc/private/processor/ipc/LAppIPC.cpp)负责传输；[CommandProcessor](../../plugins/ipc/private/processor/command/CommandProcessor.hpp)通过 `command.<name>/v1` 路由 SDK 请求并关联原连接。断线取消待决请求，但不能撤销领域已发生的副作用。GUI 传输由 IPC 插件提供，页面只使用公开 SDK 服务。

- [IpcTransportTest.cpp](../../plugins/ipc/tests/IpcTransportTest.cpp)、[IpcBoundedTransportTest.cpp](../../plugins/ipc/tests/IpcBoundedTransportTest.cpp)：真实 Unix socket 的分片、半关闭、预算、背压、析构和其他连接进展。
- [CommandGatewayTest.cpp](../../plugins/ipc/tests/CommandGatewayTest.cpp)、[JsonCommandTest.cpp](../../platform/tests/JsonCommandTest.cpp)：公开能力路由、非法输入、请求/响应关联。
- [CLI 测试](../../plugins/ipc/tests/test_ctl.py)、[DeskWaifuClientTest.cpp](../../plugins/ipc/tests/DeskWaifuClientTest.cpp)：退出码、序列化、重连、修改结果未知等客户端语义。
- [DesktopOwnershipTest.cpp](../../plugins/desktop/tests/DesktopOwnershipTest.cpp)及私有 compositor smoke：控制源仲裁、模型切换和实际命令管线。

单测或合成提供者通过不证明所有领域命令正确；本机 socket 和进程锁不是远程认证或完整安全边界。当前预算是实现约束，不对未知未来版本客户端承诺无限兼容。
