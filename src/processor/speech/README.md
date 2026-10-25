# 语音与音频播放

语音功能集中在本目录，事件适配与播放实现保持分层：

```text
speech/
  SpeechProcessor.*          业务事件入口、请求者状态、取消和完成回复
  SpeechPlayer.*             播报状态机，协调语音源与音频输出
  SpeechTypes.hpp            播报请求、格式与错误类型
  audio/AudioPlayer.*        PCM 缓冲、音频设备输出和播放排空
  source/MiniMaxSpeechSource.*  WebSocket 语音源及协议处理
  CMakeLists.txt             deskwaifu_speech 构建目标
```

- `SpeechProcessor` 属于 `deskwaifu_processors`，通过 `SpeechPlayer` 发起播报；不直接操作音频设备或解析云端协议。
- `SpeechPlayer`、`AudioPlayer` 和 `MiniMaxSpeechSource` 仍由独立的 `deskwaifu_speech` 目标构建，不反向依赖处理器或事件派发器；播放器和语音源可分别替换为测试端口。
- `AudioPlayer` 只负责 PCM 输出，不接触文本、API 密钥、OCR 或业务事件。播放方法和回调仍在 Qt 应用线程执行；没有新增音频线程或另一套播放事件协议。
- 依赖注入及生命周期顺序不变：语音源和音频播放器必须比播报协调器长寿，播报协调器必须比事件适配器长寿。现有音频/云端技术标识仍限定在播放后端内部。

外部代码使用 `processor/speech/SpeechPlayer.hpp`、`processor/speech/audio/AudioPlayer.hpp` 或 `processor/speech/source/MiniMaxSpeechSource.hpp`，不再包含 `src/` 根目录的旧路径。

## 验证

```bash
cmake --build build --target SpeechPlayer-test MiniMaxSpeechSource-test AudioPlayer-test processor-pipeline-test -j 4
ctest --test-dir build -R '^(SpeechPlayer|MiniMaxSpeechSource|AudioPlayer|processor-pipeline)$' --output-on-failure
```

默认测试使用模拟源、模拟输出、回环 WebSocket 和无效音频格式的错误路径；真实设备播放及云端合成仍是显式启用的测试，不会因目录整合自动执行。
