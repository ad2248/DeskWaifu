# 感知插件

`perception.so` 提供桌面感知能力：截图、剪贴板图像读取、图像处理和 OCR。它把感知功能从 `desktop` 渲染插件中分离，使 OCR 后端可以替换、独立测试，也避免桌面插件直接依赖 OCR SDK。

## 实现结构

- `PerceptionPlugin.cpp` 导出 SDK v1 插件入口，并创建 `PerceptionRuntime`。
- `private/processor/screenshot/ScreenshotProcessor` 负责截图源和截图处理。
- `private/processor/image/ImageProcessor` 读取剪贴板图像并执行图像处理。
- `private/processor/ocr/OcrProcessor` 管理 OCR 请求、结果和停止过程。
- `private/processor/ocr/OcrClient` 是 OCR 模块客户端；原生 OCR 模块通过独立接口加载。

初始化阶段创建 `commands` 端点并提供 `command.get_ocr_status/v1`、`command.ocr_recognize/v1`、`command.get_ocr_result/v1` 和 `command.cancel_ocr/v1`。启动阶段再创建图像、OCR 和截图处理器。命令 JSON 会先解析并校验命令名，随后转发为 `ocr.command`；收到 `ocr.result` 后按请求顺序回复调用方。

## 生命周期与依赖

图像、OCR、截图处理器由插件拥有，停止时依次停止并释放，随后清空待回复请求。插件依赖 Qt Gui/Concurrent、Plugin SDK，以及独立的截图和 OCR 客户端库。OCR 模块路径由构建配置指定，具体后端不进入插件的命令协议。

## 验证

`tests/` 覆盖 OCR 客户端契约、适配器、模块探测、截图服务和原生模块构建/加载。可使用构建目录中的 `perception` 相关测试运行回归；其中原生 OCR 和应用 smoke test 依赖对应测试模块或图形环境。
