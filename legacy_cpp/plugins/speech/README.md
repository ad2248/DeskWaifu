# 语音插件

`speech.so` 提供文本转语音、音频播放和语音设置。产品行为、凭据和验收边界见[语音与凭据](../../docs/需求描述/语音与凭据.md)；本页说明插件整体结构，私有播放模块的细节见[`private/processor/speech/README.md`](private/processor/speech/README.md)。

## 能力与入口

`SpeechPlugin.cpp` 导出 SDK v1 入口，提供：

- `speech.speak/v1`：提交文本播报请求；
- `command.get_speech_settings/v1`、`command.set_speech_settings/v1`：读取和更新语音设置；
- 订阅 `interaction.double_click/v1`：配置有效时播放问候语。

插件初始化时读取 `<configRoot>/plugin/speech/config.json`，创建 service 端点并注册能力，随后创建 `SpeechEndpoint`。收到播报请求时校验文本和 replace 参数，刷新配置后交给播放器；取消请求按 request id 传递到播报状态机。配置错误或缺少凭据只返回领域错误，不泄露 API Key。

## 实现分层

- `SpeechProcessor`：业务事件入口、请求者状态、取消和完成回复；
- `SpeechEndpoint`：能力请求到旧处理器协议的适配层；
- `SpeechPlayer`：协调语音源、替换策略、状态和完成事件；
- `MiniMaxSpeechSource`：WebSocket 语音源及协议解析；
- `AudioPlayer`：PCM 缓冲、音频输出和播放排空。

播放器、音频输出和语音源彼此解耦，测试可以注入假源和假输出。插件不把云端协议、密钥或音频设备暴露给其他插件。

## 验证

`tests/` 覆盖设置、处理器契约、端点关联/取消、MiniMax 源、音频播放和适配器失败路径。私有模块 README 中给出了典型构建和 ctest 命令；默认测试不依赖外网或真实音频设备。
