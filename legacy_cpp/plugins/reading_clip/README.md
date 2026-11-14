# 朗读片段插件

`reading_clip.so` 编排“读取图像 → OCR 识别 → 语音播报”的朗读流水线，并提供朗读状态查询。它不实现图像读取、OCR 后端、云端语音合成或音频设备播放，而是通过能力请求连接这些服务。

## 实现结构

- `ReadingPlugin.cpp` 导出插件入口，创建名为 `reading` 的端点。
- `private/processor/reading_clip/ReadingClipProcessor` 管理流水线状态、请求关联、取消和命令分发。
- 插件提供 `command.reading_clip/v1` 和 `command.get_reading_clip_status/v1`。

收到 `command.reading_clip/v1` 后，处理器进入状态机：先请求 `image.read/v1` 获取临时图像，再请求 `ocr.recognize/v1` 得到文本，最后请求 `speech.speak/v1` 播报识别结果。每一步都校验响应并在失败时回到 idle；重复触发会进入取消流程，停止时取消当前请求并使迟到结果失效。状态通过 `reading.state/v1` 通知，同时命令响应返回 queued/status JSON。

## 与其他插件的关系

IPC 或控制面板可以调用朗读命令；朗读插件不直接依赖 IPC。它通过能力端点连接感知插件的图像/OCR能力和 speech 插件的播报能力，因此可以在测试中注入旧处理器或假服务，而不改变朗读状态机。

## 验证

`tests/` 覆盖朗读流水线、事件循环和图片测试夹具。测试重点是图像→OCR→语音的顺序、状态变化、取消、失败回复和异步处理期间的生命周期安全。
