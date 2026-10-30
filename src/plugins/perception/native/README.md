# 原生 OCR 辅助程序

`deskwaifu-ocr` 是 C++/ONNX Runtime CPU 辅助进程。主程序只链接 QtCore 客户端，不链接 ONNX Runtime、Paddle 或 OpenCV。构建、安装辅助程序**不需要模型文件**；未配置模型时主程序仍可启动，OCR 状态会报告不可用原因。

本功能不包含截图采集、框选 UI 或自动朗读。只接收本机图片绝对路径，不上传图片、不自动下载/转换模型、不写入模型目录、不保存识别结果。

## 模型：直接克隆官方 ONNX 仓库

当前只支持官方 `PP-OCRv6_small_det` 与 `PP-OCRv6_small_rec` 的 ONNX 导出及其配套元数据；**不承诺兼容任意 ONNX 模型**。必须选择仓库名带 **`_onnx`** 的版本，原始 Paddle 仓库不能直接使用。无需运行本仓库历史研究用的转换脚本。

安装 Git LFS 后，由用户显式克隆到两个独立目录，例如：

```bash
mkdir -p "$HOME/.local/share/deskwaifu/ocr/models"
git clone https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx \
  "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_det"
git clone https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx \
  "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_rec"
```

目录名无需带 `_onnx` 后缀；上面是显式指定的克隆目标。若禁用了 LFS 自动获取，应在各仓库显式运行 `git lfs pull`。每个官方仓库含 `inference.onnx`、`inference.yml`、`inference.json`；辅助程序直接读取前两者，第三个是 Paddle 图描述，不是字符字典。

识别字符字典来自原始 `inference.yml` 的 `PostProcess.character_dict`，按照 PaddleX CTC 解码约定在内存中添加 blank 和空格，不生成 `dictionary.json`。不再要求自定义 manifest、校验和或 small+medium profile；应自行信任并固定所使用的上游版本。

默认目录分别为：

- `/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det`
- `/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec`

自定义目录可直接传给辅助程序（不经 shell 拼接）：

```bash
build-ocr/deskwaifu-ocr --serve \
  --detection-model-dir "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_det" \
  --recognition-model-dir "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_rec"
```

目录可以只读。旧 `--models` 单目录参数已移除。

## 构建与安装

运行依赖 glibc、C++ 运行库、Qt6 Core/Gui（含所需图片格式插件）、OpenCV core/imgproc（OpenCV 5 还需 geometry）、yaml-cpp 和 ONNX Runtime CPU。不需要 Python、Paddle、CUDA 或 Live2D SDK。模型是单独部署的数据，不属于辅助程序构建输入，也不会被辅助程序自动安装。

以下为已校验的 ONNX Runtime 1.31.0 Linux x86_64 SDK 下载方式；其他架构需使用相应 SDK 并独立核验：

```bash
mkdir -p /tmp/opencode/ocr-sdk
curl -fL --retry 3 \
  https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/onnxruntime-linux-x64-1.31.0.tgz \
  -o /tmp/opencode/ocr-sdk/onnxruntime-linux-x64-1.31.0.tgz
echo 'cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57  /tmp/opencode/ocr-sdk/onnxruntime-linux-x64-1.31.0.tgz' | sha256sum -c -
tar -xzf /tmp/opencode/ocr-sdk/onnxruntime-linux-x64-1.31.0.tgz -C /tmp/opencode/ocr-sdk

cmake -S src/plugins/perception/native -B build-ocr -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DONNXRUNTIME_ROOT=/tmp/opencode/ocr-sdk/onnxruntime-linux-x64-1.31.0
cmake --build build-ocr --parallel 4

# 只安装到私有暂存目录，不修改本机 /usr。
DESTDIR="$PWD/stage-ocr" cmake --install build-ocr --component DeskWaifuOCR
```

辅助程序通过相对 RPATH 查找随附的 ORT 运行库；**模型不再相对可执行程序定位**。暂存目录运行时应传入两个模型目录。Qt/OpenCV/yaml-cpp 是系统依赖，不随本示例打包；不能据此声称跨发行版 ABI 兼容。

## 主程序通信

开发时可用 `DESKWAIFU_OCR_HELPER=/绝对路径/deskwaifu-ocr` 覆盖辅助程序路径；该值仅为可执行程序路径，不是 shell 命令。在主程序配置根目录下的 `plugin/perception/config.json` 设置两个绝对目录（默认根目录为 `~/.config/deskwaifu`）：

```json
{"ocr_detection_model_dir":"/绝对路径/PP-OCRv6_small_det","ocr_recognition_model_dir":"/绝对路径/PP-OCRv6_small_rec"}
```

主程序客户端通过两个目录参数传递配置，并在启动时异步初始化辅助程序。`get_ocr_status` 的 `availability_state` 区分 `checking`、`available` 和 `unavailable`；`available` 只有在模型初始化成功后才为 true，`state` 仍表示识别任务状态。状态同时包含有效目录、`error_code` 和错误说明。省略的配置项使用默认值；空值、非字符串或非绝对路径报错，不展开 `~` 或环境变量，不静默回退。修改配置或修复加载错误后重启桌宠。

```bash
./deskwaifu-ctl get_ocr_status
./deskwaifu-ctl ocr_recognize --image /绝对路径/图片.png
# 接受后返回 job_id，不等待推理。
./deskwaifu-ctl get_ocr_result --job_id 上一步返回的ID
./deskwaifu-ctl cancel_ocr --job_id 上一步返回的ID
```

辅助进程启动与推理均异步进行，不应阻塞主线程。忙时拒绝新识别，不建立队列；只保留最近一个任务结果。检查中拒绝识别，不创建任务。模型缺失或加载失败会停用 OCR，重启前不反复尝试；其他能力不受影响。已成功初始化过的辅助进程因超时、协议错误或崩溃退出后，下一次用户请求可重新启动并验证模型（不是后台自动重试）。取消任务不会停用已验证的 OCR；空闲退出后也会在下一次请求重新加载。

## 辅助进程协议 v1

只通过 stdin/stdout 交换逐行 JSON，没有额外网络监听。stderr 为诊断流。模型读取、预处理元数据检查、ONNX 输入/输出类型与形状检查、字典类别数检查及一次试运行**全部成功后**才输出：

```json
{"protocol":1,"event":"ready","backend":"onnxruntime-cpu","runtime_version":"1.31.0","detection_model":"PP-OCRv6_small_det","recognition_model":"PP-OCRv6_small_rec"}
```

加载失败只输出 error 事件并非零退出，不会先报告 ready，例如：

```json
{"protocol":1,"event":"error","code":"model_missing","error":"Cannot read OCR metadata: /path/inference.yml"}
```

启动错误码：`model_missing`（文件不存在/不可读）、`model_incompatible`（元数据、预处理、类型、维度或字典不兼容）、`model_load_failed`（ONNX 无法加载）以及 `invalid_arguments`（参数错误）。

父进程请求：

```json
{"protocol":1,"id":"任务ID","image":"/绝对路径/图片.png"}
```

每个请求得到同 ID 的 `{"protocol":1,"id":"任务ID","ok":true,"result":{"width":960,"height":640,"lines":[],"elapsed_s":0.3}}` 或 `ok:false,error`。文字行含 `text`、`confidence` 和左上/右上/右下/左下四点 `polygon`（原图像素坐标）。请求错误后继续服务，stdin EOF 正常退出。

限制：单帧图片，每边不超过 4096 像素、文件不超过 64 MiB；请求行不超过 64 KiB。支持 BGR 归一化、DB quad 后处理和 CTC 解码；识别输入元数据必须为 `[3,48,320]`，实际推理宽度按比例扩展、最多 3200。检测器空间输入与识别器宽度须为动态维度。不兼容的模型在 ready 前拒绝。检测保留本 helper 的最短边 64/最长边 4000 缩放策略，不同于 PaddleX 当前 small_det 默认最长边 960；DB 阈值和扩张比例取检测模型 YAML。浮点矩形扩张与参考 pyclipper 整数偏移不逐位等价，不能声称与 PaddleX 输出完全一致。

## 验证

不下载真实权重的原生回归测试：

```bash
OCR_NATIVE_HELPER="$PWD/build-ocr/deskwaifu-ocr" \
  python -m unittest discover -s tests -p test_ocr_native_models.py -v
```

测试用标准库生成微型 ONNX fixture，覆盖分离目录（含空格/中文路径）、原始 YAML 字典及 CTC blank/空格、目录不被修改、缺失文件、损坏 YAML/ONNX、不兼容模型/预处理/静态维度/张量类型、输出类别与字典不匹配、启动错误和错误请求后恢复。未设置 `OCR_NATIVE_HELPER` 时跳过，不要求普通构建具备推理依赖。

真实模型测试（仅开发时用 Python/Pillow 生成合成图，不进入运行包）：

```bash
uv run --no-project --with pillow python research/ocr/make_samples.py \
  --font /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc --output /tmp/opencode/ocr-samples
python src/plugins/perception/tests/native_smoke.py \
  --helper build-ocr/deskwaifu-ocr --samples /tmp/opencode/ocr-samples \
  --detection-model-dir "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_det" \
  --recognition-model-dir "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_rec" \
  --output /tmp/opencode/ocr-smoke
```

真实图对比使用 NFKC 归一化并忽略空白；例如 JPEG 可能将全角 `！` 识别成 `!`，不声称逐字节一致。`app_smoke.py` 同样接受这两个模型目录参数，在私有无头合成器中验证 IPC。测试权重和产物不应提交或发布。历史 [RESULTS.md](RESULTS.md) 使用旧 small+medium 转换模型，仅作历史记录，不能作为当前 small+small 模型的准确率或性能结论。

不承诺文档排版还原、任意来源图片安全沙箱或实时看屏性能。文件由本用户授权读取；合成图验证不代表真实桌面准确率。
