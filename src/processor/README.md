# 有状态事件处理器与独立模型渲染线程

所有具体处理器在 `src/processor/` 的不同子目录中；Qt 负责等待与唤醒，`src/event/ProcessorEventLoop` 负责处理器注册、注销和派发，不再实现另一套 `exec()`。

```text
processor/
  image/          GUI 线程读取剪贴板图片，后台编码私有临时 PNG
  ocr/            OCR 进程代理 OcrClient 与事件处理器
  screenshot/     截图事件适配、独立服务接口与 portal/ 后端（见该目录 README）
  speech/         语音事件适配与播报协调，audio/ PCM 播放与 source/ 云端源
  reading_clip/   剪贴板朗读状态机
  ipc/            Unix socket 所有者、就绪事件与异步协议边界，不依赖渲染帧
  command/        CLI 协议校验与按领域有序的事件路由，不持有领域状态
  model/          模型操作代理、worker-only Cubism 生命周期与帧状态
  interaction/    GUI 交互状态所有者；含窗口、编辑事务、配置及外部控制辅助类
  input/          窗口输入与全局光标监听，只将采样转换为事件
  signal/         SIGUSR1/SIGUSR2 自管道就绪事件，不依赖渲染帧
  Processor.hpp   注册/注销生命周期基类
  Events.hpp      本阶段业务事件类型名称
  ProcessorRuntime.hpp  组装与依赖注入，不编写业务流程
```

## 辅助文件归属与迁移遗留

辅助类按职责收纳，不要求继承 Processor：

- `src/processor/model/`：所有 LApp 模型/视图/纹理/日志/资源辅助类以及 catalog/loader/renderer、Cubism 生命周期。原有独立构建目标保留。
- `src/processor/interaction/`：InteractionProcessor 拥有 InteractionContext，后者是内部 GUI 状态，不是服务定位器。ApplicationService、TransformEditor、输入手势策略和 AppCommandHandler 都是该域的内部辅助类；命令适配器只处理本域命令。
- `interaction/config/`：LAppConfig 和各 Settings 由交互域持有；持久化写入、编辑事务和窗口显示选择留在同一 GUI 所有者，避免为搬目录新增 Config/Window 空壳处理器。
- `interaction/window/`：DesktopWindow、QtDesktopWindow、输出/合成器辅助类和键盘捕获状态，由 InteractionContext 独占。
- `src/contracts/`：共享值、CPU 快照、ModelBinding、AppState、ModelExchange、InputSample、ImageFile 资源引用，以及独立 JSON/ConfigStore 协议工具。不依赖任何处理器实现，不含 GL/SDK 对象。
- `src/render/`：窗口与模型线程共同使用的离屏平台边界，包含 ModelRenderTarget、OffscreenRenderer 和像素读取；不拥有领域状态。
- `src/app/LAppDelegate` 仅创建依赖并执行 Start/Run/Stop 生命周期，不提供模型、配置、OCR 或交互业务访问器。
- 原根目录 HoverOpacity、GpuTeardown、RuntimeLifecycle 已确认无生产调用，仅旧测试引用，移至 `tests/support/`，不再伪装成运行时代码。实际悬停/销毁路径由 interaction-processor、model-render-thread 和真实组装测试覆盖。

`ModelFrame.hpp` 和 `ModelScroll.hpp` 已无生产调用，因此在补齐真实路径测试后移除：

- 原 `model-frame` 测试只运行旧 RenderModelFrame 和模拟模型。其帧调度/输入转发断言迁入 `model-render-thread`，观察真实 ModelRuntime → Manager → Model 的调用并保留正式实现：每帧恢复视线、编辑抑制、外部输入转发/清除、旧模型输入拒绝、隐藏时不更新模型但继续产出透明帧，以及重新显示后恢复更新。旧 ModelFrameTest 与构建目标一并移除。
- 原 ModelScroll 测试仅验证旧 View 适配器。替换为 `interaction-processor` 直接测试 PointerScroll：屏幕坐标转换、缩放锚点不变性、无效视口及面板编辑抑制。生产路径已不借用 View，旧 null-View 用例不再适用。
- `model-transform` 保留生产使用的变换数学、投影组合及边界测试；`model-gaze` 保留真实 SDK 眼球/嘴部参数和外部覆盖值测试；静止光标对变换、视口、半径与零半径变化的响应由 `interaction-processor` 验证。

## 统一事件模型

`processor-boundaries` 是已知跨域 include、具体处理器类型和历史回调绕道的静态护栏，不是完整的 C++ 调用图分析。任意 lambda/类型别名中的业务转调仍需代码审查；不得仅凭静态检查通过宣称所有回调均已证明安全。

```cpp
struct ProcessorEvent {
    QString type;
    IProcessor* sender = nullptr;
    IProcessor* receiver = nullptr;
    QVariantMap data;
};

bool IProcessor::HandleEvent(const ProcessorEvent& event);
```

没有事件 ID、流程 ID 或请求/响应/通知的不同派发通道。

- `receiver == nullptr`：按注册顺序调用，返回 true 即停止；需要继续传播的观察者返回 false。
- 指定接收者：只调用该处理器，即使返回 false 也不会转交给其他接收者。
- 回复时把原 sender 指针作为 receiver；不传对象所有权，不调用未注册对象。
- 新事件始终入队，不递归派发。每次唤醒处理一条；处理器内部即使运行嵌套 Qt 消息泵，也不会重入业务派发。
- 在派发中注销后面的处理器，后者不会再收到当前事件；新注册（包括同地址重新注册）的处理器从下一条事件起参与。
- 注册/注销和业务回调限主线程；Post 可由后台线程调用，但后台生产者必须遵守对象生命周期约定。
- 注销会清除队列中包含该发送者/接收者的事件。指针不会延长对象寿命；动态销毁前必须取消并收尾关联任务，停止后台投递，然后注销。**不能依靠地址比较区分销毁后同地址的新对象。**
- 回调不得抛异常或同步等待耗时子系统。排队数据使用值或共享资源引用，不持有事件自身的引用。

`OcrClient` 的既有 helper wire 协议/外部 CLI 仍保留兼容用 job_id；截图 Portal 句柄和音频流标识也仍在对应后端内部。它们不参与内部朗读流程关联。CLI 请求/结果 JSON 原样保留外部 job_id。已有 IPC connection 来源只保留在 IPC/Command 边界及 GUI 流控制/编辑事务中，不向模型/OCR/朗读领域传播，不新增任何内部关联编号。

## 剪贴板图片朗读

`ReadingClipProcessor` 自己持有状态：

```text
Idle → WaitingImage → WaitingOcr → WaitingSpeech → Idle
                  出错 → Idle
      重复触发 → Cancelling → 等待当前服务终结事件 → Idle
```

图像、OCR 和语音请求明确指定接收者，服务完成后直接回复原 sender。协调器依据当前状态与服务指针识别结果，一次只允许一个流程，不在取消完成前开始下一次。

ImageProcessor 在 GUI 线程取得剪贴板 QImage 快照，再用 QtConcurrent 编码 PNG。ImageFile 使用独占临时目录和共享所有权，保证排队事件与 OCR 使用期间文件有效，最后一个引用释放后自动清理。取消图片编码会等待任务收尾；取消 OCR 会终止 worker，并在必要时保留文件直到 worker 回收。

OcrClient 通过异步完成信号通知 OcrProcessor，后者投递事件，无状态查询轮询。语音处理器也通过服务回调投递完成/失败事件。模型双击问候已经改为语音事件；显式替换会先停止当前播报，再启动最近一次替换请求。

入口暂时复用已有 Unix socket，不增加另一条 FIFO：

```bash
# 可绑定为 KDE 自定义快捷键；必须使用本轮构建的桌宠。
./deskwaifu-ctl reading_clip
./deskwaifu-ctl get_reading_clip_status
```

第一次触发开始读取；忙时再次触发取消，取消收尾期间继续触发不会积压新任务。`queued:true` 只表示事件入队，状态及失败原因通过 `get_reading_clip_status` 查询。没有截图/剪贴板数据时明确失败；不会改为读取文字剪贴板或擅自抓取屏幕。Wayland 是否允许当前窗口读取剪贴板仍由桌面协议和焦点权限决定，不强夺焦点。

IpcProcessor 使用 QSocketNotifier 将 socket 可读/可写转换成事件；超过单次预算的已缓冲命令主动继续排队。输出窗口重建期间暂停，避免嵌套窗口事件循环重入旧模型命令。

## 验证与迁移边界

### 模型线程和操作入口

`ModelProcessor` 是 GUI 线程上的有状态操作代理；`ModelRuntime` 的创建、模型加载、动作/表情/热键/皮肤、命中测试、模型更新、GL 绘制与销毁都在同一个 `model-render` QThread 中。模型线程自己的 Qt 定时器驱动更新和绘制，不依赖主线程逐帧请求，也不向业务事件队列投递帧事件。

- 原生入口是 `model.operation`（`data.json` 为操作 JSON），worker 完成后先广播 `model.state` 不可变元数据值，再投递 `model.result`（`data.reply` 为实际结果），接收者为原 sender。已移除 Submit、SetModelChanged、Metadata 等跨域访问器与注入业务回调；交互域自行缓存状态事件。
- `CommandProcessor` 校验 JSON 并按 GUI、模型、OCR、朗读四个领域各自串行排队，领域之间可独立前进。模型动作先由 GUI 所有者通过 `interaction.model` 检查编辑约束，再保留原 sender 投递 `model.operation`，没有跨处理器函数调用。其他请求使用 `interaction.command`、`ocr.command`、`reading.command`，回复为各域的 `.result`；队列不需要请求编号。外部命令仅在实际完成后回复，不以“已入队”假装执行成功（明确异步协议的 reading_clip/ocr_recognize 除外）。
- `IpcProcessor` 独占 LAppIPC，后者只调用其所有者实现的 ICommandService；它与 CommandProcessor 通过 `command.request` / `ipc.command.result` 交换 JSON。传输层完成回调仅留在 IPC 域，不被注入其他领域。quit 回复先唤醒 IPC 刷新，再排入关闭事件，防止渲染循环退出抢在回复之前。
- CommandProcessor 不实现 ICommandService，不接受完成回调，也没有兼容性的业务直调入口；协议请求只有已入队的事件入口。
- 每个 IPC 连接最多等待一个异步回复，保留命令/回复顺序、写回压和半关闭语义；等待的连接不阻塞其他连接。完成回调只持有弱引用的回复槽，断线或 transport 销毁后不会访问旧连接或复用的 fd。
- 点击、延迟单击、滚轮、模型热键及皮肤/模型切换不访问主线程的模型对象。滚轮只在 GUI 线程计算逻辑投影；命中查询只访问不可变 CPU 快照。模型索引或实例 token 改变后，GUI 状态事件消费者清空控制来源、手势和键盘状态；失败或未改变实例不会误清空。
- `InputListener` 在 GUI 线程接收窗口输入并持有全局光标监听器，用独立 Qt 定时器采样全局光标，不依赖呈现帧；只投递定向 `input.event` / `input.move`。连续同来源移动合并为最新采样；离散事件和来源切换形成顺序屏障，不合并掉按下/释放。按钮携带采样时的单调时钟时间，排队延迟不改变双击判定。窗口表面替换期间暂停采集并清除旧输入，停止时注销源以清除未派发事件。
- `InteractionProcessor` 拥有 CursorLook、InputController、ScreenCropInteraction 和 InteractionContext；窗口、可见性、编辑事务、跨屏重绑定及呈现全部在此域内部协作。单击等待由独立 Qt 单次定时器唤醒。跨域模型操作与问候语音只发送事件；InputListener 只缓存 `window.state` 的尺寸/输出名称，不借用窗口。
- 交互初始化时发送 `model.hit_test.request`，模型处理器用 `model.hit_test.ready` 回复 `data.callback`（ModelHitTest）。缺少或失效时重新请求，同时最多保留一个待回复请求。回调只弱引用线程安全交换槽，查询最新完成渲染帧的不可变 CPU 命中快照；不访问 SDK/GL，不借用处理器。停止、析构、渲染错误或模型/视口不匹配时返回 unavailable，区别于有效的未命中。
- 命中快照同时提供 alpha 轮廓与具名 HitAreas 包围矩形（与 SDK IsHit 一致，并非逐三角形测试），无 HitAreas 时保留旧版 Head/Body 上下半区回退。轮廓命中与部位命中相互独立。交互处理器自行决定 Head/Body 点击反应、悬停透明度、裁剪区域、编辑框和窗口输入区域，渲染端只执行动作或绘制给定状态。旧 `tap_model` 操作保留为兼容入口，新交互路径不调用它。
- 当前刻意不绑定“最后成功呈现帧”，命中数据可能领先用户看到的画面，快速运动时允许轻微偏差；几何、轮廓、尺寸和模型实例标识仍来自同一个完成帧，不能混用不同模型或视口的数据。
- 原生窗口移动、InputListener 连续采样均使用有界合并槽；离散事件构成屏障。高频拖动只更新交互草稿，不为每次移动排入模型操作。呈现时用当前变换、视口和半径计算视线，通过共享 ModelExchange 最新状态槽提交；组装入口不再逐帧协调业务。
- GUI 线程只持有窗口/Wayland surface、呈现 GL context、状态及只读模型元数据。自动设备使用在 GUI 线程创建 surface、再将独立 Qt context 移至 worker 的路径；指定 PCI 设备使用独立 device-platform EGL context。GL 对象名不跨线程/设备共享。
- 帧经最多一个“最新状态”和一个“最新不可变 RGBA 帧”槽传递；慢呈现不会积压帧。主线程上传 CPU 像素后呈现，输入区域来自 worker 在应用透明度前采样的轮廓。尺寸、隐藏状态或模型索引不匹配的旧帧不会上传。现有 backend model token 仅用于拒绝旧模型的输入状态/热键，不用于事件路由。
- 销毁顺序为先停信号、IPC notifier、输入/交互生产源，取消命令并 join 模型 worker，再注销其完成事件、销毁 IPC/命令与模型代理，最后关闭窗口。能力图在注销请求者前收尾图片编码任务、OCR 子进程及其他服务；图片编码显式 waitForFinished，OCR kill/join 后才释放临时文件。Qt surface 销毁限 GUI，模型与 GL 析构限 worker 当前 context。
- `SignalProcessor` 的 POSIX handler 只向非阻塞自管道写字节；主线程的 QSocketNotifier 转为普通事件。显示切换和可见性信号不再由呈现帧检查全局标志。

这是正确性优先的 CPU readback/upload 路径，**不是零拷贝或性能优化**；自动设备也会承担像素传输开销，尚未提供跨 GPU 零拷贝保证。GUI 卡住时 worker 可以继续更新模型，但窗口呈现/配置/输入仍受 GUI 线程调度约束。

`processor-event-loop` 验证顺序消费、定向派发、异步/非递归队列、跨线程投递、注销清理及同指针重新注册。

`processor-pipeline` 用合成图片、模拟 OCR 子进程和模拟播报端验证完整流程、取消、故障恢复、临时文件释放；实际语音代理只测试缺少凭据的错误路径，不连接云端或播放声音。另验证无任何渲染帧时 IPC 的读取、缓冲命令继续处理、写回压和断开清理。截图服务继续使用私有 D-Bus 模拟测试。

`model-render-thread` 使用真实 Cubism 与隔离 EGL，验证线程归属、GUI 不派发时仍持续绘制、最新帧覆盖/不可变性、操作结果、模型切换、失败初始化及有序销毁。`command-processor`/`signal-processor` 验证没有呈现帧时的命令和信号派发；`ipc-bounded-transport` 补充延迟回复、半关闭、其他连接进展及晚到完成测试。原有实际模型点击、热键和跨屏拖动测试保留并改为通过操作事件访问模型。

`interaction-processor` 不创建 GL 上下文、不读取真实桌面输入、不访问语音服务，验证无渲染帧时延迟单击、双击/拖拽互斥、模式取消、静止光标重算、输入合并顺序、hover 策略、原始输入时间与处理器销毁。`model-hit-test` 验证快照轮廓/部位独立性、多部位重叠、失效及并发查询；`model-render-thread` 验证真实模型回调握手和停止失效。`model-pointer-transform` 将快照与实际 SDK 命中测试对照（含变换、竖屏、视图矩阵与配置区域），`model-pointer-release` 使用真实模型验证无呈现帧时的定时单击。

`processor-ownership` 连接真实 IpcProcessor → CommandProcessor → Interaction/Ocr/Reading 所有者；只替换窗口/模型/播报平台边界，使用私有 socket 和合成图片/native OCR fixture，验证两个连接在慢模型操作期间独立前进、编辑状态授权、元数据状态缓存、原生输入两级有界合并、半关闭管线、完整朗读及销毁传输后的晚完成。SpeechProcessor 在生产构造器中独占 source/audio/player，配置变化通过事件更新，不从 Delegate 借用这些业务对象。

**仍保留的边界：**

- 配置/窗口/外部控制的业务实现仍复用现有 ApplicationService、TransformEditor 与命令适配层，而不是为每个配置字段新增处理器；入口已经由 CommandProcessor 排队。窗口状态采样、编辑器 debounce 与呈现仍使用 GUI 定时调度。
- `processor-boundaries` 静态检查处理器实现的直接/相对跨域 include、前置声明具体同级类型、Delegate 服务定位器及旧业务回调；负例验证规则确实拒绝这些回归。测试夹具的友元访问只用于内部状态准备/断言，生产头不提供业务访问器。
- OCR 旧外部 wire/CLI 通过命令适配层保留兼容；不把 job_id 引入内部流程。
- 尚未增加独立 FIFO 监听或全局热键注册；KDE 可调用现有 CLI 入口。
- 未在用户真实剪贴板、真实 Portal 或真实云端播报上做自动测试；这些需要用户主动授权验证。

后续性能工作应单独度量 CPU 帧传输与 OCR 推理瓶颈，不把模型绘制或业务流程重新塞进 ProcessorRuntime。
