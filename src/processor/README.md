# 有状态事件处理器与独立模型渲染线程

所有具体处理器在 `src/processor/` 的不同子目录中；Qt 负责等待与唤醒，`src/event/ProcessorEventLoop` 负责处理器注册、注销和派发，不再实现另一套 `exec()`。

```text
processor/
  image/          GUI 线程读取剪贴板图片，后台编码私有临时 PNG
  ocr/            OCR 进程代理 OcrClient 与事件处理器
  screenshot/     系统截图服务的事件适配
  speech/         语音播放服务的事件适配
  reading_clip/   剪贴板朗读状态机
  ipc/            Unix socket 就绪事件，不依赖渲染帧
  command/        配置、窗口控制及兼容 CLI 命令的有序事件入口
  model/          模型操作代理、worker-only Cubism 生命周期与帧状态
  signal/         SIGUSR1/SIGUSR2 自管道就绪事件，不依赖渲染帧
  Processor.hpp   注册/注销生命周期基类
  Events.hpp      本阶段业务事件类型名称
  ProcessorRuntime.hpp  组装与依赖注入，不编写业务流程
```

## 统一事件模型

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

`OcrClient` 的既有 helper wire 协议/外部 CLI 仍保留兼容用 job_id；截图 Portal 句柄和音频流标识也仍在对应后端内部。它们不进入 ProcessorEvent，不参与内部路由或朗读流程关联。后端技术标识不等于新增一套业务事件 ID。

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

- 原生入口是 `model.operation`（`data.json` 为操作 JSON），完成后投递 `model.result`（`data.reply` 为结果），接收者为原 sender。`ModelProcessor::Submit` 是现有 CLI/输入回调的兼容适配；worker 完成后投递 `model.completed`，在主线程更新元数据快照并回复。事件仍是同一个 `ProcessorEvent`，不新增关联 ID；共享操作数据持有兼容适配所需的完成回调。
- `CommandProcessor` 将所有 Unix socket 命令/断开通知排入统一事件队列；配置、窗口和外部控制状态留在 GUI 线程，模型操作再交给 `ModelProcessor`。外部命令只在操作完成后返回真实结果，不以“已入队”假装执行成功。
- 每个 IPC 连接最多等待一个异步回复，保留命令/回复顺序、写回压和半关闭语义；等待的连接不阻塞其他连接。完成回调只持有弱引用的回复槽，断线或 transport 销毁后不会访问旧连接或复用的 fd。
- 点击、延迟单击、滚轮、模型热键及皮肤/模型切换不再访问主线程的模型对象。滚轮只在 GUI 线程计算逻辑投影；实际命中测试在 worker 上执行。模型切换成功且索引实际改变后才清空控制来源、待处理手势和旧键盘状态；失败或选中原模型不会误清空。
- GUI 线程只持有窗口/Wayland surface、呈现 GL context、状态及只读模型元数据。自动设备使用在 GUI 线程创建 surface、再将独立 Qt context 移至 worker 的路径；指定 PCI 设备使用独立 device-platform EGL context。GL 对象名不跨线程/设备共享。
- 帧经最多一个“最新状态”和一个“最新不可变 RGBA 帧”槽传递；慢呈现不会积压帧。主线程上传 CPU 像素后呈现，输入区域来自 worker 在应用透明度前采样的轮廓。尺寸、隐藏状态或模型索引不匹配的旧帧不会上传。现有 backend model token 仅用于拒绝旧模型的输入状态/热键，不用于事件路由。
- 销毁顺序是停止信号/IPC 入口、停止并 join worker、清除其排队完成事件、销毁命令/业务处理器，最后销毁主线程窗口。Qt 的 offscreen surface 创建/销毁限 GUI 线程；模型与 GL 析构限 worker 当前 context。
- `SignalProcessor` 的 POSIX handler 只向非阻塞自管道写字节；主线程的 QSocketNotifier 转为普通事件。显示切换和可见性信号不再由呈现帧检查全局标志。

这是正确性优先的 CPU readback/upload 路径，**不是零拷贝或性能优化**；自动设备也会承担像素传输开销，尚未提供跨 GPU 零拷贝保证。GUI 卡住时 worker 可以继续更新模型，但窗口呈现/配置/输入仍受 GUI 线程调度约束。

`processor-event-loop` 验证顺序消费、定向派发、异步/非递归队列、跨线程投递、注销清理及同指针重新注册。

`processor-pipeline` 用合成图片、模拟 OCR 子进程和模拟播报端验证完整流程、取消、故障恢复、临时文件释放；实际语音代理只测试缺少凭据的错误路径，不连接云端或播放声音。另验证无任何渲染帧时 IPC 的读取、缓冲命令继续处理、写回压和断开清理。截图服务继续使用私有 D-Bus 模拟测试。

`model-render-thread` 使用真实 Cubism 与隔离 EGL，验证线程归属、GUI 不派发时仍持续绘制、最新帧覆盖/不可变性、操作结果、模型切换、失败初始化及有序销毁。`command-processor`/`signal-processor` 验证没有呈现帧时的命令和信号派发；`ipc-bounded-transport` 补充延迟回复、半关闭、其他连接进展及晚到完成测试。原有实际模型点击、热键和跨屏拖动测试保留并改为通过操作事件访问模型。

**仍保留的边界：**

- 配置/窗口/外部控制的业务实现仍复用现有 ApplicationService、TransformEditor 与命令适配层，而不是为每个配置字段新增处理器；入口已经由 CommandProcessor 排队。窗口状态采样、编辑器 debounce 与呈现仍使用 GUI 定时调度。
- OCR 旧外部 wire/CLI 通过命令适配层保留兼容；不把 job_id 引入内部流程。
- 尚未增加独立 FIFO 监听或全局热键注册；KDE 可调用现有 CLI 入口。
- 未在用户真实剪贴板、真实 Portal 或真实云端播报上做自动测试；这些需要用户主动授权验证。

后续性能工作应单独度量 CPU 帧传输与 OCR 推理瓶颈，不把模型绘制或业务流程重新塞进 ProcessorRuntime。
