# 处理器代码归属

本目录不再组装业务处理器。项目自有运行时代码按 SDK、平台和插件划分：

- [SDK](../../sdk/README.md)：`IPlugin`、`IGuiPlugin`、`IProcessorPlugin`、公共事件和通信契约。
- [插件架构](../../docs/plugin-architecture.md)：宿主、插件发现、生命周期和依赖方向。
- [桌面插件](../plugins/desktop/README.md)：模型、渲染、窗口、交互、输入和信号。
- `src/plugins/perception/private/processor/`：图像、OCR、截图服务。
- `src/plugins/ipc/private/processor/`：通用 IPC 传输与命令网关。
- `src/plugins/speech/private/processor/speech/`：语音、音频和提供者。
- `src/plugins/reading_clip/private/processor/reading_clip/`：剪贴板朗读编排。

处理器是插件内部的状态所有者，不是平台中的内置业务服务。跨插件协作使用 SDK
公开契约；平台不包含具体处理器头文件，不链接其实现，也不持有业务对象。

插件内部的指针路由仍通过共享 FIFO 保证非递归派发及注册令牌失效。跨插件能力请求
使用独立 RequestId，具有明确的响应、取消、超时、无提供者和提供者冲突语义。

测试归属、迁移证据和自动验证范围见 [测试文档](../../docs/testing.md)。
