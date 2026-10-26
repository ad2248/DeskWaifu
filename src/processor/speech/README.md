# 语音与音频播放

语音功能集中在本目录，事件适配与播放实现保持分层：

```text
speech/
  SpeechProcessor.*          业务事件入口、请求者状态、取消和完成回复
  SpeechEndpoint.*           插件内部的能力请求到旧处理器适配，按 request_id 关联
  SpeechPlayer.*             播报状态机，协调语音源与音频输出
  SpeechTypes.hpp            播报请求、格式与错误类型
  audio/AudioPlayer.*        PCM 缓冲、音频设备输出和播放排空
  source/MiniMaxSpeechSource.*  WebSocket 语音源及协议处理
  CMakeLists.txt             deskwaifu_speech 构建目标
```

- `SpeechProcessor` 通过 `SpeechPlayer` 发起播报；不直接操作音频设备或解析云端协议。它与 `SpeechEndpoint` 由语音模块私有使用，不在宿主组装语音适配器。
- `SpeechPlayer`、`AudioPlayer` 和 `MiniMaxSpeechSource` 保持独立的播放分层；播放器和语音源可分别替换为测试端口，不重写已有状态机。
- `AudioPlayer` 只负责 PCM 输出，不接触文本、API 密钥、OCR 或业务事件。播放方法和回调仍在 Qt 应用线程执行；没有新增音频线程或另一套播放事件协议。
- 依赖注入及生命周期顺序不变：语音源和音频播放器必须比播报协调器长寿，播报协调器必须比事件适配器长寿。现有音频/云端技术标识仍限定在播放后端内部。

外部代码使用 `processor/speech/SpeechPlayer.hpp`、`processor/speech/audio/AudioPlayer.hpp` 或 `processor/speech/source/MiniMaxSpeechSource.hpp`，不再包含 `src/` 根目录的旧路径。

## 动态插件

`src/plugins/speech/SpeechPlugin.cpp` 导出 SDK v1 入口。它提供 `speech.speak/v1`、`command.get_speech_settings/v1`、`command.set_speech_settings/v1`，并订阅 `interaction.double_click/v1`；双击问候和 replace 策略只存在于插件。能力正文使用共享的 `ProcessorEvent::type/data`，领域常量见 `contracts/SpeechEvents.hpp`。

`SpeechEndpoint` 复用 `SpeechProcessor`，不额外注册能力，也不自发起一次能力请求。可选 `request_id` 随旧处理器的终态回复传递；取消按指针及 ID 匹配，既能移除待启动替换，又不会使迟到取消打断新播报。不含该字段的旧调用继续使用原有指针协议。

配置由 `SpeechSettings` 管理，路径为 `<configRoot>/plugin/speech/config.json`。缺少凭据允许加载但播报返回领域失败；损坏配置使初始化失败，运行中损坏也拒绝使用缓存密钥或覆盖文件。设置回复只包含 `ok/configured/error`，不返回或广播密钥。

新增的 `src/plugins/speech/tests/SpeechPluginLoadTest.cpp` 接收真实 `speech.so` 路径，使用 PluginManager 验证加载和私有配置；`SpeechEndpointTest.cpp` 使用假源和假音频验证关联、取消、超时、替换及停止。两者都不需要外网或音频设备。

## 验证

```bash
cmake --build build --target SpeechPlayer-test MiniMaxSpeechSource-test AudioPlayer-test processor-pipeline-test -j 4
ctest --test-dir build -R '^(SpeechPlayer|MiniMaxSpeechSource|AudioPlayer|processor-pipeline)$' --output-on-failure
```

默认测试使用模拟源、模拟输出、回环 WebSocket 和无效音频格式的错误路径；真实设备播放及云端合成仍是显式启用的测试，不会因目录整合自动执行。
