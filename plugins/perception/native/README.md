# 进程内原生 OCR 模块

产品范围、能力组合与验收边界见[图像识别与剪贴板朗读](../../../docs/需求描述/图像识别与剪贴板朗读.md)；本页保留模型部署、构建与 helper 协议细节。

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

目录名无需带 `_onnx` 后缀；上面是显式指定的克隆目标。若禁用了 LFS 自动获取，应在各仓库显式运行 `git lfs pull`。每个官方仓库含 `inference.onnx`、`inference.yml`、`inference.json`；模块直接读取前两者，第三个是 Paddle 图描述，不是字符字典。

识别字符字典来自原始 `inference.yml` 的 `PostProcess.character_dict`，按照 PaddleX CTC 解码约定在内存中添加 blank 和空格，不生成 `dictionary.json`。不再要求自定义 manifest、校验和或 small+medium profile；应自行信任并固定所使用的上游版本。

默认目录分别为：

- `/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det`
- `/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec`

安装 OCR 组件会创建空的 `${CMAKE_INSTALL_PREFIX}/share/deskwaifu/ocr/models` 目录（安装前缀为 `/usr` 时即上述路径），但不会下载或安装模型。目录可以只读。自定义目录使用下述主程序配置，不再通过辅助程序命令行传递。

## 构建与安装

运行依赖 glibc、C++ 运行库、Qt6 Core/Gui（含所需图片格式插件）、OpenCV core/imgproc（OpenCV 5 还需 geometry）、yaml-cpp 和 ONNX Runtime CPU。不需要 Python、Paddle、CUDA 或 Live2D SDK。模型是单独部署的数据，不属于模块构建输入，也不会被自动安装。整个项目可用 `-DDESKWAIFU_BUILD_OCR=OFF` 跳过推理依赖；这不影响主程序和 perception 插件构建。

开发、构建和运行统一使用系统包管理器提供的 ONNX Runtime，不下载或捆绑独立 SDK。开发包必须提供 `onnxruntimeConfig.cmake` 和 `onnxruntime::onnxruntime` target。

发布历史版本时，若其进程内 OCR 构建规则仍依赖独立 SDK，发布配方会在解包后的构建副本应用系统依赖补丁；不会修改原标签或恢复 SDK 下载、运行库捆绑。

Arch Linux 示例：

```bash
sudo pacman -S --needed opencv yaml-cpp onnxruntime-cpu

cmake -S plugins/perception/native -B build-ocr -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build-ocr --parallel 4

# 只安装到私有暂存目录，不修改本机 /usr。
DESTDIR="$PWD/stage-ocr" cmake --install build-ocr --component DeskWaifuOCR
```

安装内容为 `usr/lib/deskwaifu/ocr/deskwaifu-ocr.so`、模块文档及模型许可证，不包含 ORT 运行库副本、ORT 许可证副本、可执行辅助程序或模型。模块通过系统动态链接器加载 ORT；模型仍使用配置中的绝对路径。Qt/OpenCV/yaml-cpp/ONNX Runtime 都是系统依赖，正式 OCR 包声明 `onnxruntime-cpu` 运行依赖；模块与主程序需匹配 Qt/C++ ABI 和私有接口版本，正式 OCR 包严格依赖同版本主包。不能据此声称跨发行版 ABI 兼容。

## 配置与升级

默认模块路径为 `/usr/lib/deskwaifu/ocr/deskwaifu-ocr.so`（可由安装前缀/libdir 改变）。开发时可用 `DESKWAIFU_OCR_MODULE=/绝对路径/deskwaifu-ocr.so` 指向暂存模块。旧 `DESKWAIFU_OCR_HELPER` 已不再读取，也不会回退到子进程。升级时同步更新主包及 OCR 包，已有模型目录配置无需迁移。

在主程序配置根目录下的 `plugin/perception/config.json` 设置两个绝对目录（默认根目录为 `~/.config/deskwaifu`）：

```json
{"ocr_detection_model_dir":"/绝对路径/PP-OCRv6_small_det","ocr_recognition_model_dir":"/绝对路径/PP-OCRv6_small_rec"}
```

启动时异步初始化模型。`get_ocr_status` 的 `availability_state` 区分 `checking`、`available` 和 `unavailable`；`available` 只有在模型初始化成功后才为 true，`state` 仍表示识别任务状态。状态同时包含有效目录、`error_code` 和错误说明。省略的配置项使用默认值；空值、非字符串或非绝对路径报错，不展开 `~` 或环境变量，不静默回退。修改配置或修复加载错误后重启桌宠。

```bash
./deskwaifu-ctl get_ocr_status
./deskwaifu-ctl ocr_recognize --image /绝对路径/图片.png
# 接受后返回 job_id，不等待推理。
./deskwaifu-ctl get_ocr_result --job_id 上一步返回的ID
./deskwaifu-ctl cancel_ocr --job_id 上一步返回的ID
```

忙时拒绝新识别，不建立队列；只保留最近一个任务结果。检查中拒绝识别，不创建任务。模型缺失或加载失败会停用 OCR，重启前不反复尝试；其他能力不受影响。成功初始化过的引擎因超时或无效响应退役后，可由下一次用户请求重新加载（不是后台自动重试）。

## 生命周期与接口兼容

原有四个外部命令、任务 ID、`pending/completed/failed`、识别结果字段及 `response.protocol/id` 保留。内部不再使用 stdin/stdout 协议，而是版本化的 Qt 模块工厂接口；模型元数据、形状、字典和试运行全部通过后才标记可用。

- `execution_mode` 固定为 `in_process`。
- `running` 表示模型工作实例已加载或正在初始化、推理、释放，不再表示子进程存在。
- `busy` 表示底层正在执行操作，不能接收新任务。
- `retiring` 表示取消、超时或空闲释放后的收尾阶段。
- 默认初始化/任务超时为 60 秒，模型在空闲 60 秒后于后台释放；下次请求重新加载。专用线程可保持休眠，不代表模型仍驻留。释放资源不保证分配器立即将全部 RSS 归还操作系统。
- 取消立即将用户任务标记为 `failed`；超时也只终结用户任务。底层调用可能继续执行，其迟到结果会丢弃。期间仍拒绝新任务，输入文件及模块代码保留到真实执行结束，不强杀线程。
- 应用退出或插件停用时停止接收任务，等待执行结束再卸载代码、释放输入文件。若原生推理永久卡死，退出也可能等待；无安全的强制线程终止或子进程重启恢复能力。

错误码与处理：

- `module_missing`：安装匹配版本的 OCR 功能包。
- `module_incompatible`：模块格式或接口版本不符，重新安装匹配包。
- `module_load_failed`：检查 Qt/OpenCV/yaml-cpp/ORT 依赖及 ABI。
- `invalid_config`：修正配置 JSON 和绝对目录。
- `model_missing`、`model_incompatible`、`model_load_failed`：检查官方 ONNX 模型、元数据和文件权限。
- `timeout`、`backend_error`：等待 `busy/retiring` 清除后，已验证的引擎可重新请求。

文字行仍含 `text`、`confidence` 和左上/右上/右下/左下四点 `polygon`（原图像素坐标），结果保留 `width/height/lines/elapsed_s`。可捕获的异常返回失败，不影响其他功能。**不再具备进程崩溃隔离：原生库段错误、abort 或内存破坏可能使整个应用退出。** 模型与图像不是安全沙箱输入，只应使用可信文件。日志不默认记录图像或识别全文。

限制：单帧图片，每边不超过 4096 像素、文件不超过 64 MiB。支持 BGR 归一化、DB quad 后处理和 CTC 解码；识别输入元数据必须为 `[3,48,320]`，实际推理宽度按比例扩展、最多 3200。检测器空间输入与识别器宽度须为动态维度。不兼容的模型在可用前拒绝。检测保留最短边 64/最长边 4000 缩放策略，不同于 PaddleX 当前 small_det 默认最长边 960；DB 阈值和扩张比例取检测模型 YAML。浮点矩形扩张与参考 pyclipper 整数偏移不逐位等价，不能声称与 PaddleX 输出完全一致。

## 验证

不下载真实权重的原生回归测试：

```bash
OCR_NATIVE_MODULE="$PWD/build-ocr/deskwaifu-ocr.so" \
OCR_MODULE_PROBE="$PWD/build-ocr/ocr-module-probe" \
  python -m unittest discover -s tests -p test_ocr_native_models.py -v
```

测试用标准库生成微型 ONNX fixture，覆盖分离目录（含空格/中文路径）、原始 YAML 字典及 CTC blank/空格、目录不被修改、缺失文件、损坏 YAML/ONNX、不兼容模型/预处理/静态维度/张量类型、输出类别与字典不匹配、启动错误和错误请求后恢复。未设置上述环境变量时跳过。`ocr-module-probe` 仅在 `BUILD_TESTING=ON` 时构建，不安装；其 JSON 行输入只是测试驱动，不是正式运行模式。

无 OCR 依赖的生命周期测试：

```bash
cmake -S plugins/perception/tests/ocr-client -B build-ocr-client -G Ninja
cmake --build build-ocr-client
ctest --test-dir build-ocr-client --output-on-failure
```

真实模型测试（需要自备合成图样本目录，样本不进入运行包）：

```bash
python plugins/perception/tests/native_smoke.py \
  --probe build-ocr/ocr-module-probe --module build-ocr/deskwaifu-ocr.so \
  --samples <样本目录> \
  --detection-model-dir "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_det" \
  --recognition-model-dir "$HOME/.local/share/deskwaifu/ocr/models/PP-OCRv6_small_rec" \
  --output /tmp/opencode/ocr-smoke
```

真实图对比使用 NFKC 归一化并忽略空白；例如 JPEG 可能将全角 `！` 识别成 `!`，不声称逐字节一致。`app_smoke.py --app … --module … --live2d-models … --samples … --logs …` 同样接受两个模型目录参数，在私有无头合成器中验证 IPC、模块映射、无 OCR 子进程及取消后恢复。测试权重和产物不应提交或发布。历史 [RESULTS.md](RESULTS.md) 使用旧 small+medium 转换模型，仅作历史记录，不能作为当前模型的准确率或性能结论。

### 真实模型样本目录

`--samples` 指向自备样本目录，`native_smoke.py` 与 `app_smoke.py` 共用同一套文件；样本不进入仓库或发布包。样本可用任意绘图工具生成，例如用 Pillow 以 CJK 字体绘制同一组文字并另存为 JPEG，不需要专门的生成脚本。

`native_smoke.py` 按固定文件名发起请求，目录必须同时满足：

| 文件 | 要求 |
| --- | --- |
| `expected.json` | 必需。JSON 对象，键为样本文件名，值为该图的期望文字行数组；必须含 `light.png`。 |
| `light.png` | 必需。单帧 PNG，识别结果须与 `expected.json` 的 `light.png` 项一致；`app_smoke.py` 另断言恰好 3 行。 |
| `light.jpg` | 必需。内容同 `light.png`，只作 JPEG 解码对照，期望文本取 `light.png` 项。 |
| `blank.png` | `app_smoke.py` 必需。无文字，`expected.json` 对应项为 `[]`。 |
| `dense.png` | `app_smoke.py` 必需。多行文字图，用于取消后恢复场景，内容不参与断言。 |
| `oversized.png` | 必需。任一边超过 4096 像素的单帧图片，断言被拒绝；不必写入 `expected.json`。 |
| `animated.gif` | 必需。多帧 GIF，断言被拒绝；不必写入 `expected.json`。 |
| `missing.png` | 必须不存在，用于断言缺失文件被拒绝。 |

`expected.json` 的每个键都会按文件名请求 3 次，键名必须是目录中真实存在的图片，未列出的图片不会被识别；`missing.png`、`oversized.png`、`animated.gif` 不得作为键。比较时行序与空白不敏感：逐行 NFKC 归一化并去除全部空白后排序比较，重复行计数仍参与。最小示例：

```json
{
  "light.png": [
    "姐姐，你好！桌宠 OCR 本地测试。",
    "DeskWaifu OCR 1234567890",
    "CPU only / no cloud upload"
  ],
  "blank.png": []
}
```

样本须为模型可识别的单帧高对比度图片，并遵守每边 4096 像素、文件 64 MiB 的限制。`native_smoke.py` 只向探针传递 `PATH` 和 `LANG`，不继承 `LD_LIBRARY_PATH` 等变量，系统动态链接器须能直接找到 ONNX Runtime。测试还会复制识别模型目录并清空 `inference.yml` 的 `character_dict`，要求以 `model_incompatible` 拒绝，故该字段必须保留。

不承诺文档排版还原、任意来源图片安全沙箱或实时看屏性能。文件由本用户授权读取；合成图验证不代表真实桌面准确率。

本次迁移的测试范围、性能对照及局限见 [进程内化验证记录](../../../docs/ocr-in-process-validation.md)。
