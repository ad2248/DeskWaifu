# 本机 OCR 实验

独立的 PP-OCRv6 CPU 调试目录，**不是桌宠正式功能**。不接入 CMake、CTest、CI、Arch Linux 包或运行时依赖；所有命令手动执行。不采集屏幕、不调用云端、不自动朗读。

原生可选功能包及服务实现位于 [perception/native](../../src/plugins/perception/native/README.md)。本目录归属 perception 插件的开发工具，保留 Python 基线、模型导出与张量对照工具；`export` 依赖组仅用于开发机，不随原生功能包分发。

## 当前验证状态（2026-10-22）

- 已通过：10 项不依赖 Paddle 的单元测试、Python 编译检查、模型路径缺失时的 JSON 错误与非零退出码。
- 已生成：uv 锁文件，包含 PaddleOCR 3.7.0、PaddleX 3.7.2、PaddlePaddle 3.3.1。
- 用户手动安装后，`uv sync --locked --offline` 已确认环境完整；两组检测器搭配 medium_rec 的四类合成图片均已完成真实推理，忽略空白后按行内容集合比较均匹配（保留重复行计数）。
- 两组 `--serve` 均已验证 ready、8 次请求（含 5 次错误）、错误后恢复、JPEG/PNG、纯 JSONL stdout 和 EOF 正常退出。修复了 JPEG 无 `n_frames` 属性导致的原型错误并补充回归测试。
- 性能测量及局限见 [RESULTS.md](RESULTS.md)。尚未验证真实桌面图片、持续运行内存、MKL-DNN/GPU 优化或桌宠集成。

锁文件与本地模型组合已通过本轮合成图验证，不代表生产环境和任意图片均可用。

## 环境

在此目录执行，使用 uv 和 Python 3.12，避免系统 Python 版本与 Paddle wheel 不兼容：

```bash
cd research/ocr
uv sync --locked
```

模型只从显式提供的本地目录加载，不复制进仓库。目录应含：

```text
PPOCR/
  PP-OCRv6_small_det/{inference.json,inference.pdiparams,inference.yml}
  PP-OCRv6_medium_det/{inference.json,inference.pdiparams,inference.yml}
  PP-OCRv6_medium_rec/{inference.json,inference.pdiparams,inference.yml}
```

`small_det + medium_rec` 为默认候选；`--detector medium` 用于对照。关闭文档方向分类、矫正和文字行方向分类，避免额外模型。当前禁用 MKL-DNN，先建立基础 CPU 可运行基线，不能把它当成最优性能。

## 单图与性能探测

```bash
mkdir -p output
uv run ocr_probe.py --models-root /home/kals/下载/PPOCR \
  --image /绝对路径/截图.png --repeat 3 > output/result.json
```

标准输出为一个 JSON 对象，日志走标准错误。输出包含：

- `load_s`：PaddleOCR 导入与模型初始化时间，不含 Python/uv 启动。
- `runs[0].elapsed_s`：首次推理；后续运行是同一进程复用模型的热推理，不含文件头校验，包含实际推理时的图片读取。
- `rss_mib`：每次推理结束后的进程 RSS。
- `process_peak_rss_mib`：Linux 进程累计峰值 RSS，包含导入、初始化和之前的所有请求，不是单次请求增量。
- `lines`：文字、模型置信度、原图像素坐标下的四边形。不承诺多栏阅读顺序或排版还原。

默认 `--threads 2`、`--max-side 4096`。超限图片直接拒绝，避免隐式缩放导致坐标含义不清。只支持单帧图片。模型内部仍可调整检测分辨率并将结果映射回原图。

本地研究结果可能含隐私文字和文件路径，请将重定向输出放在已忽略的 `output/` 中，不提交真实截图和识别内容。

## 可复现的无隐私样例

```bash
uv run make_samples.py --font /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc
uv run ocr_probe.py --models-root /home/kals/下载/PPOCR \
  --image output/samples/light.png --repeat 3 > output/small-light.json
uv run ocr_probe.py --models-root /home/kals/下载/PPOCR --detector medium \
  --image output/samples/light.png --repeat 3 > output/medium-light.json
```

字体路径需按本机调整。另生成 `dark-small.png`、`dense.png`、`blank.png` 和 `expected.json`，用于检查小字号、深色背景、密集文本与空白图。**合成图通过只证明基本链路可用，不代表真实桌面准确率**。比较检测器时分别启动新进程，保持图片、线程数和重复次数一致，避免混淆累计内存峰值。

## 长驻辅助进程原型

```bash
uv run ocr_probe.py --models-root /home/kals/下载/PPOCR --serve
```

初始化成功输出一行 `{"event":"ready",...}`。随后从 stdin 逐行读取：

```json
{"id":"request-1","image":"/绝对路径/截图.png"}
```

每行返回 `{"id":"request-1","ok":true,"result":{...}}`，错误返回同一 ID 与 `ok:false,error`。请求串行执行，EOF 退出；普通请求错误不会结束进程。可用于后续验证 `QProcess` 接口，但**本轮没有修改桌宠接入它**。

此原型只用于可信本机输入，不是网络服务或安全沙箱。暂未实现请求超时、强制取消、空闲退出、截图授权或 UI；原生推理崩溃仍会使辅助进程退出，未来应由父进程处理生命周期。不要将它当成生产服务启动。

## 手动测试

```bash
uv run python -m unittest discover -s . -p 'test_*.py' -v
```

单元测试不加载 Paddle 或模型；真实模型验证按上面的命令独立执行。依赖与模型下载需要网络，而实际识别使用本地模型。后续正式集成前，还需要真实桌面样例、内存/延迟预算，以及 Wayland portal 授权和多屏缩放验证。
