# IPC 插件

`ipc.so` 是 DeskWaifu 面向外部程序的命令网关。它把本地 IPC 传输中的请求转换为 Plugin SDK 的命令能力请求，并把插件响应转换回外部客户端；业务插件不需要知道 socket 协议细节。

## 实现结构

- `IpcPlugin.cpp` 负责组装 `CommandProcessor` 和 `IpcProcessor`，并控制启动/停止顺序。
- `private/processor/ipc/LAppIPC` 实现底层 IPC 传输与 socket 路径管理。
- `IpcProcessor` 管理传输生命周期、就绪状态、读取和写回。
- `CommandProcessor` 将外部命令映射到 Plugin SDK 的 `command.<name>/v1` 能力，并处理请求关联、取消和边界。
- GUI 构建开启时，`gui/DeskWaifuClient` 和 `IpcGuiPlugin` 提供控制面板侧客户端集成。

启动时先创建命令处理器，再创建 IPC 处理器；只有传输成功启动后插件才报告 ready。停止时先停止传输，再停止命令处理器，确保不会继续接收新请求或向已释放的端点回调。`deskwaifu-ctl` 是面向用户的命令行客户端。

## 协作关系

IPC 插件不实现模型、OCR、语音或朗读业务。外部请求经命令总线发送到各插件提供的能力，例如 desktop 的模型/交互命令、perception 的 OCR 命令和 speech 的设置命令。命令响应统一通过 `command.result` 返回，领域插件仍拥有自己的校验和错误语义。

## 验证

`tests/` 覆盖传输边界、就绪状态、上下文生命周期、命令处理器、命令网关和 DeskWaifu 客户端。重点验证半包/有界传输、取消、停止期间请求以及无效命令不会破坏 IPC 状态。
