# Live ASR 合成音频

`voice-input.pcm` 为无文件头的 PCM16 LE / 16000 Hz / 单声道合成语音，不是真人录音：

> Hello world. This is a voice input test.

由以下命令生成；只在显式授权的 live ASR 验证中上传，不用于普通离线门禁：

```bash
espeak-ng -s 145 -w voice-input.wav 'Hello world. This is a voice input test.'
ffmpeg -i voice-input.wav -ar 16000 -ac 1 -f s16le voice-input.pcm
```

`voice-input-live-test` 使用生产 NativeAsrClient 向 `BAILIAN_ASR_ENDPOINT` 指定的 WSS 端点请求固定模型 `qwen-audio-3.1-asr-flash-streaming`（凭据取自 `BAILIAN_API_KEY`），校验结果包含 hello、world、voice、input、test（忽略标点/大小写），而不是只检查非空。不录音、不访问剪贴板，不打印凭据或响应全文。
