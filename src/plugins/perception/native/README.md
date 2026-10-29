# 可选 OCR 功能包（实验阶段）

`deskwaifu-ocr` 是单独构建和安装的 C++/ONNX Runtime CPU 辅助进程。主程序只链接一个 QtCore 客户端，不链接 ONNX Runtime、Paddle 或 OpenCV；未安装功能包时仍正常启动。

本轮不提供截图采集、框选 UI 或自动朗读。只通过已有桌宠 IPC 接收**本机图片绝对路径**。不自动上传或下载模型，不保存识别结果到磁盘。

## 包含与不包含

功能包包含辅助程序、ONNX Runtime CPU 运行库、`PP-OCRv6_small_det + PP-OCRv6_medium_rec` 的 ONNX 模型、CTC 字典、模型来源/转换版本/SHA-256 清单，以及许可证声明。启动时校验模型与字典 SHA-256。

运行依赖系统的 glibc、C++ 运行库、Qt6 Core/Gui 和 OpenCV core/imgproc（OpenCV 5 还需 geometry），包括所需图片格式的 Qt 插件。**不包含 Python、Paddle、CUDA、Live2D SDK，也不打包 glibc/Qt/OpenCV。** battery included 指无需配置推理环境或另行下载模型，不是跨发行版通用静态程序。

当前提供本机开发用 TXZ 安装归档，并非已发布的 Arch/pacman 包。未改动现有 Arch 主包和 CI；二进制只保证本机已验证的系统 ABI。发布前需要干净环境验证和独立 Arch 包元数据。

## 构建

在仓库根目录执行。Python **只在开发机转换模型时使用**，不会进入原生功能包。

```bash
# CMake 4 构建旧版 onnxoptimizer 时需要兼容策略；限制并行避免过量编译。
CMAKE_ARGS='-DCMAKE_POLICY_VERSION_MINIMUM=3.5' \
CMAKE_BUILD_PARALLEL_LEVEL=4 MAX_JOBS=4 \
uv sync --project research/ocr --locked --group export

uv run --project research/ocr --group export research/ocr/export_onnx.py \
  --models-root /home/kals/下载/PPOCR \
  --output research/ocr/output/onnx-models

uv run --project research/ocr --group export research/ocr/verify_onnx.py \
  --models-root /home/kals/下载/PPOCR \
  --onnx-root research/ocr/output/onnx-models
```

获取 ONNX Runtime 1.31.0 C/C++ CPU SDK（已验证 Linux x86_64）：

```bash
mkdir -p research/ocr/output/sdk
curl -fL --retry 3 \
  https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/onnxruntime-linux-x64-1.31.0.tgz \
  -o research/ocr/output/sdk/onnxruntime-linux-x64-1.31.0.tgz
echo 'cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57  research/ocr/output/sdk/onnxruntime-linux-x64-1.31.0.tgz' | sha256sum -c -
tar -xzf research/ocr/output/sdk/onnxruntime-linux-x64-1.31.0.tgz -C research/ocr/output/sdk

cmake -S src/plugins/perception/native -B build-ocr -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DONNXRUNTIME_ROOT="$PWD/research/ocr/output/sdk/onnxruntime-linux-x64-1.31.0" \
  -DOCR_MODELS_DIR="$PWD/research/ocr/output/onnx-models"
cmake --build build-ocr --parallel 4

# 只安装到私有暂存目录，不修改本机 /usr。
DESTDIR="$PWD/research/ocr/output/stage" cmake --install build-ocr --component DeskWaifuOCR
cpack --config build-ocr/CPackConfig.cmake -B research/ocr/output/packages
```

归档按 `/usr` 前缀包含 `usr/libexec/deskwaifu-ocr`、`usr/lib/deskwaifu/ocr/`、`usr/share/deskwaifu/ocr/` 和许可证。辅助程序用相对 RPATH 查找自带 ORT，按相对安装路径查找模型，因此解压至私有目录也能运行。应用的常规 `DeskWaifu` 安装组件不会包含本功能包。

正式安装需要单独部署归档内容到对应前缀；本轮**没有自动安装系统包，也没有替换正在运行的桌宠**。

## 主程序通信

主程序编译时记录 `${CMAKE_INSTALL_FULL_LIBEXECDIR}/deskwaifu-ocr`。开发时可在启动本轮新构建的桌宠前显式指定：

```bash
DESKWAIFU_OCR_HELPER="$PWD/research/ocr/output/stage/usr/libexec/deskwaifu-ocr" \
  build-ocr-app/bin/deskwaifu
```

这个覆盖只接受程序路径，不通过 shell 执行字符串。图片路径也通过 JSON 传输，不拼接命令行。

调用示例：

```bash
./deskwaifu-ctl get_ocr_status
./deskwaifu-ctl ocr_recognize --image /绝对路径/图片.png
# 返回 {"ok":true,"job_id":"...","state":"pending"}，不等待推理。
./deskwaifu-ctl get_ocr_result --job_id 上一步返回的ID
./deskwaifu-ctl cancel_ocr --job_id 上一步返回的ID
```

- `get_ocr_status`：返回 `available`（辅助程序存在且可执行）、`running`、最近任务状态和 ID。available 不代表模型校验或运行库加载一定成功。
- `ocr_recognize`：按需拉起辅助进程，握手成功后提交任务。忙时拒绝，不建立任务队列。未安装包、不可读路径等在接受前返回 `ok:false`。
- `get_ocr_result`：状态为 `pending/completed/failed`。结束后含 `response`，其中 `ok:true,result` 或 `ok:false,error` 表示**识别任务**结果；最外层 `ok:true` 仅代表查询成功。只保留最近一个任务，接受新任务后旧 ID 变为 unknown。
- `cancel_ocr`：只取消匹配的 pending ID，强制终止辅助进程，记录 failed；不会误取消新任务。

整个启动加推理的请求期限为 60 秒。成功/任务错误后复用辅助进程，空闲 60 秒退出，释放推理资源。超时、取消、协议错误和崩溃后终止或回收进程，下一次请求可重新启动；终止尚未完成时返回稍后重试，不进行隐式重试。正常识别期间主线程不等待子进程或磁盘图片解码。

限制：单帧图片，每边不超过 4096 像素，文件不超过 64 MiB；辅助进程请求行不超过 64 KiB，主程序接收结果不超过 4 MiB。结果仅含原图像素坐标、文字、置信度与处理时间，不承诺文档排版还原。

## 辅助进程协议 v1

辅助进程仅通过父子进程 stdin/stdout 通信，没有额外网络监听或用户服务。stderr 为诊断流，主程序及时排空，不无限缓存日志。

启动 `deskwaifu-ocr --serve` 后先输出一行：

```json
{"protocol":1,"event":"ready","backend":"onnxruntime-cpu","runtime_version":"1.31.0","profile":"ppocrv6-small-medium-v1"}
```

父进程提交一行：

```json
{"protocol":1,"id":"任务ID","image":"/绝对路径/图片.png"}
```

每个请求得到同 ID 的 `{"protocol":1,"id":"任务ID","ok":true,"result":{"width":960,"height":640,"lines":[],"elapsed_s":0.3}}` 或 `ok:false,error`。每个文字行含 `text`、`confidence` 和按左上/右上/右下/左下顺序的四点 `polygon`。请求错误后继续服务；模型加载失败输出 error 事件并非零退出；stdin EOF 正常退出。

## 手动验证

无需推理依赖的客户端生命周期测试（独立配置，不加入现有 CI）：

```bash
cmake -S src/plugins/perception/tests/ocr-client -B build-ocr-tests -G Ninja
cmake --build build-ocr-tests
ctest --test-dir build-ocr-tests --output-on-failure
```

真实模型测试，先生成样例：

```bash
uv run --project research/ocr research/ocr/make_samples.py \
  --font /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc \
  --output research/ocr/output/samples
python src/plugins/perception/tests/native_smoke.py \
  --helper research/ocr/output/stage/usr/libexec/deskwaifu-ocr \
  --samples research/ocr/output/samples --output research/ocr/output/native-smoke
```

主程序端到端测试在私有无头合成器运行，不接触当前桌面、当前配置或正在运行的实例。需要已构建的桌宠、SDK 样例模型及 Sway 或 KWin：

```bash
python src/plugins/perception/tests/app_smoke.py --compositor kwin \
  --app build-ocr-app/bin/deskwaifu \
  --helper research/ocr/output/stage/usr/libexec/deskwaifu-ocr \
  --live2d-models /原仓库路径/CubismSdkForNative-5-r.5/Samples/Resources \
  --samples research/ocr/output/samples --logs research/ocr/output/app-smoke
```

验证结果与性能限制见 [RESULTS.md](RESULTS.md)。当前逐行识别实现比 Python 基线慢，**尚不能宣称适合实时看屏或性能优于 Paddle**。没有为任意来源图片提供安全沙箱；文件由本用户授权读取。
