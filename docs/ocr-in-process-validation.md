# OCR 进程内化验证记录

日期：2026-11-01。实现分支：`refactor/ocr-in-process`。

## 实现边界

- 正式路径由 `OcrClient` 在专用线程加载可选 `deskwaifu-ocr.so`，不再启动 helper，也没有进程外回退路径。
- 引擎初始化、单任务推理、释放均在该线程执行；工厂接口版本为 `org.deskwaifu.OcrBackendFactory/1.0`，每个客户端独立持有模型实例。
- 保留四个 OCR 命令、任务 ID、结果结构和默认模型目录；`running` 改为工作实例状态，增加 `busy`、`retiring`、`execution_mode`。仅在实际初始化成功后报告可用。
- 保留 60 秒超时和 60 秒空闲释放。取消/超时先结束用户任务，底层执行结束前继续拒绝任务、保留输入文件，丢弃迟到结果。
- 插件停用/应用退出时等待后台调用结束，再释放输入及卸载模块。不会强杀线程；原生崩溃可能带退出应用，永久卡死可能阻止正常退出。
- 主包不直接链接 ORT/OpenCV/yaml-cpp；OCR 包继续可选、严格依赖同版本主包，不包含模型。旧 helper 环境变量不再生效。

配置、构建、升级和故障处理见 [原生 OCR 模块说明](../plugins/perception/native/README.md)。

## 自动化及集成验证

- 完整 Release 构建及 CTest：147 项，146 项通过，`offscreen-renderer` 因环境跳过。
- 独立客户端测试通过 ASan/UBSan，覆盖同 PID/后台线程、模型复用、空闲重新加载、无队列 busy、取消、超时、迟到结果、初始化/识别/释放期间销毁、异常、无效结果、独立会话和循环释放。
- 缺失模块、缺失动态依赖和错误接口版本均只禁用 OCR，不使测试宿主退出。
- 原生模型契约测试包含 16 个用例，使用微型 ONNX fixture，覆盖元数据、字典、CTC、错误模型和十次加载/识别/释放。
- 感知适配器测试检查取消后的临时图像在实际推理结束前仍然存在，结束后删除。
- 私有 KWin virtual 会话中的真实主程序完成：无功能包启动、有效模型初始化、实际识别、busy 拒绝、取消后恢复及正常退出。检查模块位于主程序 `/proc/PID/maps`，没有 OCR helper 子进程。推理期间一次 `get_status` 响应约 0.3 ms，非延迟保证。
- 分别暂存安装两个组件并检查实际文件布局：主组件 24 个文件，OCR 组件 10 个文件；不安装 helper、测试宿主或模型。ELF 检查确认 perception 没有 ORT/OpenCV/yaml-cpp 直接依赖，OCR 模块使用 `$ORIGIN` 查找同目录 ORT。
- 暂存安装后的模块在不设置 `LD_LIBRARY_PATH` 的环境中通过真实模型 smoke，包括 JPEG、空白图、非法图片和错误字典。

`ocr-module-probe` 是不安装的测试宿主，不是第二套生产运行模式。测试将空闲时间缩短为 20 ms；产品默认值仍为 60 秒。

## 一致性与性能对照

基线为 `main` 提交 `0058b7e7889a97b64a766108fc0fa362450f912b` 的独立 helper；新实现使用测试宿主加载模块。两者均为同机 GCC 16.2.1 Release 构建，CPU 为 Intel Core Ultra 7 270K Plus，OpenCV 5.0.0，ORT 1.31.0，保持原有 CPU 线程和算法参数。不是完整桌宠的总资源开销对比。

使用相同 PP-OCRv6 small 检测/识别模型及合成样本，模型 SHA-256：

- det：`d73e0058b7a8086bbd57f3d10b8bcd4ff95363f67e06e2762b5e814fe9c9410e`
- rec：`5435fd747c9e0efe15a96d0b378d5bd157e9492ed8fd80edf08f30d02fa24634`

四类 PNG 各三次及一张 JPEG，共 13 次识别，排除 `elapsed_s` 后的尺寸、行顺序、文本、置信度、框坐标完全一致。

热推理取每图后两次的均值，单位 ms，单轮观测：

| 样本 | 原 helper | 进程内模块 |
| --- | ---: | ---: |
| light | 152.81 | 156.62 |
| dark-small | 132.92 | 140.60 |
| dense | 421.22 | 426.47 |
| blank | 79.06 | 85.24 |

进程创建到模型可用共测五次（文件系统缓存未清空），中位数：原 helper 155.40 ms，新测试宿主 155.47 ms。一次 smoke 的宿主峰值 RSS 分别约 516.1 MiB 和 516.2 MiB；后续暂存布局运行约 364.1 MiB，说明单轮内存观测存在较大波动，不能宣称节省内存。

新测试宿主一轮识别后的 `/proc` RSS 约从 310.6 MiB 降至空闲释放后的 98.4 MiB。真实模型连续一百次加载/识别/释放均成功，空闲 RSS 在约 96.8–182.2 MiB 间波动，最终约 106.9 MiB，后五十次的最高值低于前五十次。未观察到持续线性增长，但不构成长时无泄漏证明。分配器、线程和动态库缓存不保证释放模型后 RSS 回到初始值；旧 helper 退出则不再占用该进程内存。

结论：本次交付是进程与生命周期收敛，不是推理加速或内存优化。结果一致；实测时延接近但有波动，不据此承诺跨机器性能。

## 复现及未覆盖范围

构建和 smoke 命令见模块说明。客户端 sanitizer 构建可增加：

```bash
cmake -S plugins/perception/tests/ocr-client -B /tmp/opencode/ocr-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-gnu-unique' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_MODULE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/opencode/ocr-asan
ctest --test-dir /tmp/opencode/ocr-asan --output-on-failure
```

本次未修改系统安装、用户配置或正在运行的桌宠；未下载模型、上传图像、提交或推送代码。未执行新版本正式 Arch 发布/干净容器安装，也未对任意真实桌面图片、其他架构或数小时运行建立保证。已发布的 AUR 子模块仍指向历史版本，不在本次本地实现中改写。
