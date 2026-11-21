# Rust 版 Live2D 重写方案

## 范围与架构

首版以 Linux x86_64 / Wayland 为目标，在现有 Rust 宿主中加载 `terminal` 动态插件。以旧版的模型发现、加载切换、动作表情、点击穿透、移动缩放、显隐和输出选择作为首批行为基线。裁剪编辑、控制面板、合成器专用功能和指定 GPU 分批恢复。旧版行为以需求和归档实现为准，迁移时为命令、配置字段和模型资源分别建立对照表。 [模型交互与显示](<../docs/需求描述/模型交互与显示.md>) · [旧版桌面插件](<../legacy_cpp/plugins/desktop/README.md>)

`plugins/terminal` 负责模型展示、交互和设置，包括模型目录、动画控制、交互状态、配置事务和命令。插件的专用线程管理 Wayland 连接、surface 和 EGL，在该线程创建、使用和销毁 Cubism 对象。主线程继续运行宿主的 Tokio 调度和 IPC。控制面板作为独立进程通过现有 Unix socket 操作插件。 [Rust 架构与 IPC](<../README.md#L20>) · [宿主事件循环](<../crates/os/src/host.rs#L14>)

## 1. 将 Cubism Native SDK 接入 Rust

采用 **独立 `cubism-bridge` crate + 小型 C ABI shim**。C++ 侧保留官方 Framework 的模型、动作、物理、遮罩和 OpenGL 渲染器。Rust 侧通过 `cubism-bridge` 的封装调用，负责应用策略。桥接层只暴露必要的接口，便于审查资源所有权和调用线程。

- **接口。** C++ shim 暴露运行时初始化、模型加载/释放、元数据查询、动作/表情等参数设置、`update(dt)`、`draw(target, viewport, transform)` 和销毁。使用不透明句柄、定宽数值、指针加长度、调用方缓冲区和状态码。异常在 C++ 侧捕获并转成错误。帧、错误文本、模型元数据在边界复制或写入调用方缓冲区。Rust 安全包装校验尺寸、stride、长度、时间和变换，将 `unsafe` 集中在桥接及本地图形绑定。SDK 对象与当前 GL context 绑定，Rust 句柄为 `!Send + !Sync`。 [工作区 unsafe 策略](<../Cargo.toml#L15>)
- **生命周期。** 原型先限制为进程内一套 Cubism Framework 生命周期。加载新模型成功后才替换当前模型和会话代号，失败时保留旧模型。退出先关闭命令入口和帧源，再唤醒 worker，在所属线程使 context current 后销毁模型、纹理及渲染目标，随后销毁 EGL 和 Wayland 资源。worker 完成且线程回收后方可卸载插件。启动、加载、显示连接出错时，通过插件状态返回失败并清空交互区域，让宿主继续处理其他插件的命令。 [旧版呈现与停止](<../legacy_cpp/plugins/desktop/README.md#L20>) · [Rust 插件生命周期](<../README.md#L22>)
- **构建。** PoC 固定旧版使用的 Native SDK `5-r.5`，由显式构建配置传入 SDK 目录，使用 Cargo `build.rs` 驱动小型 CMake 工程编译 shim 和 Framework。验证目标架构、Core 静态库能否链接进 `cdylib`、C++ runtime、GL loader、shader 部署及兼容性 profile。以真实 SDK 测试为准，旧版使用 `CSM_TARGET_LINUX_GL`，EGL 创建 context 后要验证 Cubism 实际用到的着色器和顶点数组路径。默认 workspace 构建走替身和纯 Rust 测试，显式启用的原生构建缺 SDK 时清楚报错。发布前核对 Core、Framework、样例模型和用户可扩展应用的分别适用条款，明确二进制包和私有 CI 的分发方式。 [旧版构建配置](<../legacy_cpp/plugins/desktop/CMakeLists.txt#L1>) · [Live2D SDK 发布许可](https://www.live2d.com/en/sdk/license/)

### 代码示例：从原生模型到 Rust 渲染线程

下面只展示 `初始化 → 加载 → 更新/绘制 → 销毁` 的最小接口。`NativeRuntime` 和 `NativeModel` 是拟实现的 C++ 适配类，不是 Cubism 自带类型。模型目录、动作和表情控制留在后续接口。示例采用 Linux 本地路径字节，C++ 侧构造文件路径时使用给定长度，不要求 UTF-8 或 NUL 结尾。

**C 头文件：固定 ABI，Cubism 类型不出边界。**

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DwCubism DwCubism;
typedef struct DwModel DwModel;
typedef int32_t DwStatus;

enum { DW_OK = 0, DW_INVALID = 1, DW_NATIVE = 2 };

// 调用者保证同一 GL context 在所属线程上 current。
// 初始化/加载失败时，非空 out 对应的输出置为 NULL。
DwStatus dw_cubism_create(DwCubism **out);
DwStatus dw_model_load(DwCubism *runtime, const uint8_t *path,
                       size_t path_len, DwModel **out);
// framebuffer 由调用方在同一 context 内绑定并清屏。
DwStatus dw_model_frame(DwModel *model, float dt, int32_t width, int32_t height);
// 销毁须持有有效 context；模型必须先于 runtime 销毁。
void dw_model_destroy(DwModel *model);
void dw_cubism_destroy(DwCubism *runtime);

#ifdef __cplusplus
}
#endif
```

**C++ shim：以加载和逐帧调用为例。** `NativeRuntime` 封装 Framework 启停和单实例检查，`NativeModel` 封装 `.model3.json`、纹理、动作和 Cubism renderer。加载用 RAII 回滚部分创建的对象；SDK 返回错误时由适配器转换成失败，不能把半初始化句柄交给 Rust。每个返回状态码的 ABI 入口均采用相同的异常边界，销毁路径必须不抛异常。

```cpp
#include "dw_cubism.h"
#include "native_model.hpp" // 本项目将实现的 NativeRuntime / NativeModel
#include <cmath>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

struct DwCubism { NativeRuntime runtime; };
struct DwModel { std::unique_ptr<NativeModel> model; };

extern "C" DwStatus dw_cubism_create(DwCubism** out) {
    if (!out) return DW_INVALID;
    *out = nullptr;
    try {
        *out = new DwCubism{}; // NativeRuntime 构造时启动 Framework；失败则抛出
        return DW_OK;
    } catch (...) {
        return DW_NATIVE;
    }
}

extern "C" DwStatus dw_model_load(DwCubism* rt, const uint8_t* path,
                                  size_t len, DwModel** out) {
    if (!out) return DW_INVALID;
    *out = nullptr;
    if (!rt || !path || len == 0 || std::memchr(path, 0, len)) return DW_INVALID;
    try {
        auto model = rt->runtime.load(std::string_view{
            reinterpret_cast<const char*>(path), len});
        if (!model) return DW_NATIVE;
        *out = new DwModel{std::move(model)};
        return DW_OK;
    } catch (...) {
        return DW_NATIVE;
    }
}

extern "C" DwStatus dw_model_frame(DwModel* model, float dt,
                                   int32_t width, int32_t height) {
    if (!model || !std::isfinite(dt) || dt < 0 || width <= 0 || height <= 0)
        return DW_INVALID;
    try {
        model->model->update(dt);
        model->model->draw_into_bound_framebuffer(width, height);
        return DW_OK;
    } catch (...) {
        return DW_NATIVE;
    }
}

extern "C" void dw_model_destroy(DwModel* model) { delete model; }
extern "C" void dw_cubism_destroy(DwCubism* rt) { delete rt; }
```

**Rust 包装：仅在桥接内部调用 FFI。** `load` 返回借用 `Runtime` 的 `Model`，编译器阻止先销毁运行时；`Rc` 标记阻止句柄被移动或共享到其他线程。线程绑定并不等于 context 已绑定，因此初始化和绘制保留显式的 GL 安全前置条件；对上层插件应由渲染 worker 集中满足这些条件。

```rust
use std::{marker::PhantomData, os::unix::ffi::OsStrExt, path::Path, ptr::NonNull, rc::Rc};

mod ffi {
    #[repr(C)]
    pub struct DwCubism { _opaque: [u8; 0] }
    #[repr(C)]
    pub struct DwModel { _opaque: [u8; 0] }

    unsafe extern "C" {
        pub fn dw_cubism_create(out: *mut *mut DwCubism) -> i32;
        pub fn dw_model_load(rt: *mut DwCubism, path: *const u8, len: usize,
                             out: *mut *mut DwModel) -> i32;
        pub fn dw_model_frame(model: *mut DwModel, dt: f32, width: i32, height: i32) -> i32;
        pub fn dw_model_destroy(model: *mut DwModel);
        pub fn dw_cubism_destroy(rt: *mut DwCubism);
    }
}

#[derive(Debug)]
pub enum Error { InvalidInput, Native(i32), NullHandle }

type Result<T> = std::result::Result<T, Error>;

fn check(code: i32) -> Result<()> {
    if code == 0 { Ok(()) } else { Err(Error::Native(code)) }
}

pub struct Runtime {
    raw: NonNull<ffi::DwCubism>,
    _local: PhantomData<Rc<()>>,
}

pub struct Model<'rt> {
    raw: NonNull<ffi::DwModel>,
    _runtime: &'rt Runtime,
}

impl Runtime {
    /// # Safety
    /// 当前线程必须绑定目标 GL context，且从成功返回到 Runtime/Model 析构
    /// 结束期间，不得解绑、切换或销毁该 context。真实驱动丢失走显式失败清理。
    pub unsafe fn attach_to_current_gl() -> Result<Self> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::dw_cubism_create(&mut raw) })?;
        Ok(Self { raw: NonNull::new(raw).ok_or(Error::NullHandle)?, _local: PhantomData })
    }

    pub fn load(&self, model_json: &Path) -> Result<Model<'_>> {
        let path = model_json.as_os_str().as_bytes();
        if path.is_empty() || path.contains(&0) { return Err(Error::InvalidInput); }
        let mut raw = std::ptr::null_mut();
        // FFI 同步读取 path，不保留借用；成功后所有权交给 Model。
        check(unsafe {
            ffi::dw_model_load(self.raw.as_ptr(), path.as_ptr(), path.len(), &mut raw)
        })?;
        Ok(Model { raw: NonNull::new(raw).ok_or(Error::NullHandle)?, _runtime: self })
    }
}

impl Model<'_> {
    /// # Safety
    /// 当前绑定的 framebuffer 必须完整、尺寸匹配，并在调用期间有效。
    pub unsafe fn render_into_bound_fbo(&mut self, dt: f32, width: i32, height: i32) -> Result<()> {
        if !dt.is_finite() || dt < 0.0 || width <= 0 || height <= 0 {
            return Err(Error::InvalidInput);
        }
        check(unsafe { ffi::dw_model_frame(self.raw.as_ptr(), dt, width, height) })
    }
}

impl Drop for Model<'_> {
    fn drop(&mut self) { unsafe { ffi::dw_model_destroy(self.raw.as_ptr()) } }
}

impl Drop for Runtime {
    fn drop(&mut self) { unsafe { ffi::dw_cubism_destroy(self.raw.as_ptr()) } }
}
```

**worker 调用：** EGL 及帧循环方法用伪代码表示。创建和销毁都发生在 worker 内，宿主只能发送命令和接收状态，不能拿到原生句柄。示例覆盖 context 有效的路径。实际实现遇到 context 丢失时，必须先尝试恢复绑定；无法恢复时，通过显式收尾路径返回错误，不能在无有效 context 时照常调用上述 GL 析构。

```rust,ignore
fn run_worker(egl: &mut WorkerEgl, model_json: &Path) -> WorkerResult<()> {
    let mut gl = egl.make_current()?;
    {
        // 此作用域内不切换 context；结束时 model → runtime → gl。
        let runtime = unsafe { Runtime::attach_to_current_gl()? };
        let mut model = runtime.load(model_json)?;
        while let Some(frame) = gl.next_frame_or_stop()? {
            gl.bind_and_clear_model_fbo(frame.width, frame.height)?;
            unsafe { model.render_into_bound_fbo(frame.dt, frame.width, frame.height)? };
            gl.present_with_input_region(&frame)?;
        }
    }
    Ok(())
}
```

### 命令与渲染线程

先用现有 `on_command` 提供状态查询和有界任务投递。查询读最新快照。切模型等耗时操作返回已受理的操作 ID，由查询命令区分进行中、完成、失败和队列已满。控制面板首版通过现有 `source=cli` 字符串命令传输，使用结构化结果，等出现需要长时间等待、取消或跨插件调用的场景，再引入相应宿主能力。停止信号独立于业务队列，保证队列满时仍能唤醒 worker。按键、按下/释放和编辑命令保持顺序，鼠标移动和连续状态允许合并。 [宿主命令与调度契约](<../README.md#L26>) · [JSON socket 协议](<../README.md#L81>)

每帧携带模型会话代号、输出及视口代号。渲染线程从模型自身 alpha 提取命中区域，再应用最终透明度和裁剪。视觉、逻辑坐标输入区域和命中几何使用同一待提交帧，提交成功后再更新本地命中快照。模型切换、输出重建、可见性变化和渲染失败都会使旧代号失效。显示采用 latest-value 快照，慢消费者跳帧。隐藏暂停动画帧并清空模型输入，屏幕编辑有独立的输入策略。 [旧版交互约束](<../docs/需求描述/模型交互与显示.md#L19>) · [旧版轮廓算法](<../legacy_cpp/plugins/desktop/private/processor/model/ModelInputRegion.hpp#L6>)

## 2. GUI：GPUI Kit

独立控制面板采用 [GPUI Kit（`gpui-kit`）](https://github.com/longbridge/gpui-kit)，使用其 GPUI 应用入口和组件实现模型列表、动作与表情按钮、配置表单和运行状态。面板拥有自己的事件循环，通过现有 Unix socket 异步提交命令并查询结果。关闭面板不影响桌宠，面板不加载 Cubism 或访问原生模型句柄。原型阶段验证面板在 Linux Wayland 上的构建与启动、中文字体和输入法，以及 Tab/Shift+Tab、文本编辑与模型快捷键的优先级。

插件通过 `Plugin::plugin_id` 声明稳定、可读的英文 ID，例如 `terminal`。各插件在初始化时注册自己的标签页。面板通过 `panel-tabs` 查询并按注册顺序值、插件 ID、标签页 ID 排序呈现。标签页的全局标识为 `<plugin-id>/<tab-id>`，插件初始化失败时回滚其注册。

宿主统一管理 `$XDG_CONFIG_HOME/deskwaifu/config.jsonc`，未设置绝对 XDG 路径时使用 `~/.config/deskwaifu/config.jsonc`。根对象的一级键是插件 ID。插件通过 SDK 读写所属节点，宿主原子替换文件并保留其他节点和注释。共享文件使用 `0600` 权限。

桌宠 overlay 是专用呈现后端，由 `smithay-client-toolkit`（SCTK）、`wayland-client` 和 EGL/OpenGL 负责，独立于 GPUI Kit。保留这条路径是为了由 `terminal` 插件控制 layer-surface 角色、区域级点击穿透，以及画面和输入区域的同步提交。原型先用 `wl_shm` 验证透明、输入和缩放，再接真实 Cubism。正式呈现优先验证同线程 EGL window surface + FBO，保留 CPU readback 基线作正确性和性能对照。 [SCTK layer-shell](https://smithay.github.io/client-toolkit/smithay_client_toolkit/shell/wlr_layer/index.html)

overlay 默认复现旧版 overlay 层、`exclusive_zone=-1`、键盘交互 `None`。初次空 commit 后等待 configure 并 ack，再附加 buffer。用 `wl_surface.set_input_region` 提交逻辑坐标矩形集合。整窗穿透时传显式空 region，`NULL` 表示默认输入区域。尺寸和输出变化时同步重新计算逻辑尺寸、缓冲区大小、视口和命中坐标。分数缩放使用 `fractional-scale` + `viewporter` 实测，协议缺失时使用整数缩放路径。 [layer-shell 协议](https://wayland.app/protocols/wlr-layer-shell-unstable-v1) · [旧版窗口配置](<../legacy_cpp/plugins/desktop/private/processor/interaction/window/QtDesktopWindow.cpp#L80>)

GL 路径使用同一 Wayland 连接创建 surface 和 EGL native window，在交换缓冲提交对应帧前设置输入 region，由同一次 surface commit 应用。验证预乘 alpha、遮罩混合和上下原点。命中轮廓先复用旧版数量级的块采样并合并矩形，基准记录帧时间、回读字节、CPU 占用与 IPC 延迟。高分辨率下若回读拖慢输入或渲染，优先从 FBO 生成低分辨率 alpha 掩码。若实际驱动的交换缓冲持续阻塞事件处理，再拆为窗口线程和独立 GL worker，继续使用有界控制通道和最新帧交接。 [旧版轮廓算法](<../legacy_cpp/plugins/desktop/private/processor/model/ModelInputRegion.hpp#L6>)

## 3. 平台需要提供的系统能力

| 能力 | 平台与后端的职责 | 缺失时的行为 |
| --- | --- | --- |
| overlay、输入区域、输出/seat 事件 | 必需。Wayland 会话提供 `zwlr_layer_shell_v1`，适配器负责 configure、帧回调、seat 指针和按需键盘交互、输出枚举/标识/热插拔与缩放。编辑结束、隐藏或失败时恢复默认焦点策略。 | 状态报告 overlay 不可用，普通宿主与 CLI 保持可用。 |
| 图形上下文及缓冲 | 必需。平台提供 EGL/GL 驱动和带 alpha 的配置。适配器管理 EGL window surface，支持重建、帧节流与失联清理。 | 上报 context、surface 或渲染失败，清空输入，保留宿主。 |
| 选择输出及焦点输出 | 选择指定连接器使用 Wayland 输出信息。焦点输出额外接 Hyprland/Sway IPC，限制查询时间和响应大小。重绑使用明确的输出 ID 并等待 configure，保存和回滚由插件处理。 | 保留显式输出选择和热插拔回退。焦点查询不可用时命令报告原因。 |
| 全局光标 | KWin 的会话 D-Bus/临时脚本提供全局视线跟随，需要处理过期采样、重连和崩溃后清理。Hyprland 的光标查询用于恢复旧版跨输出拖动。 | 视线降级为表面局部指针，拖动留在当前输出。 |
| 全局快捷键 | 用户合成器绑定调用现有 CLI。需要 UI 管理时再接 GlobalShortcuts portal，探测后端和用户授权。面板内的模型快捷键由面板处理。 | CLI 和面板仍可控制，快捷键状态报告可用性。 |
| 指定渲染 GPU | 后续恢复 EGL device 枚举、DRM render node/PCI 映射和离屏 context，保存选择并在重启时生效。应用报告活动设备和待重启值。 | `auto` 独立可用。显式设备启动失败时报告错误并保留用户选择。 |

合成器适配器按能力探测返回可用、未支持或当前失败。overlay 与 EGL 后端首版由 `terminal` 插件内部拥有，便于在同一线程协调 buffer 与 input region 提交。`crates/os` 和公共 SDK 只在存在复用方时增加窄系统服务，例如全局快捷键或只读焦点输出。服务接口传输出标识、逻辑坐标、时间戳等 Rust 类型。当前 `OsServices` 提供录音和剪贴板，Live2D 首版沿用现有命令注册、调度和停止接口。后续语音联动可把归一化口型值推给模型，双击只通知语音侧。 [旧版 Wayland 集成](<../docs/需求描述/Wayland集成.md>) · [当前系统服务](<../crates/os/src/services.rs#L17>) · [GlobalShortcuts portal](https://docs.rs/ashpd/latest/ashpd/desktop/global_shortcuts/index.html)

## 实施顺序与验收

当前 `terminal` 的呈现路径为 `wl_shm`。私有 Sway 上的 EGL 对照原型验证了真实 Cubism 像素和同次提交的输入区域。在 1600² 下，Intel DG1 的帧处理 p95 从 `wl_shm` 的 16.130 ms 降到 EGL 的 3.197 ms，llvmpipe 则从 28.859 ms 增至 37.497 ms。EGL 显式回读为每帧 160,000 字节，`wl_shm` 为 10,240,000 字节。对照例程测量渲染线程 socket 回显。使用 `wl_shm` 的真实宿主在私有 Sway 中执行 150 次状态查询，覆盖模型加载和连续渲染，往返 p95 为 1.240 ms，最大为 1.751 ms。首版保留 `wl_shm`，EGL window surface 的插件集成需验证分数缩放、输出重建和上下文丢失。 [复现步骤与数据](<../crates/cubism-bridge/README.md>)

### 旧版配置迁移

用 [迁移脚本](<../scripts/migrate-live2d-config.py>) 读取归档的桌面插件配置。输出是包含 `terminal` 键的 JSON，可把该节合并到共享 JSONC。已有 `terminal` 节时按字段合并。脚本只读源文件，诊断写到 stderr。

```sh
python3 scripts/migrate-live2d-config.py LEGACY_CONFIG \
  --model-directory /absolute/model/catalog \
  --startup-model /absolute/model/catalog/Haru/Haru.model3.json
```

启动输出映射到 `output`，渲染设备映射到 `gpu`。平时和悬停透明度、注视半径、普通表情超时及裁剪区域可直接迁移。旧版负数表情超时统一转换为 `-1`。旧版位置和缩放依赖当时的视口和模型投影，需要在新版预览后手动调整。脚本会列出无对应项的设置，包括视口提示尺寸和旧插件目录。目录及启动模型选项用于明确旧版多个目录或模型文件夹名的映射。

| 阶段 | 交付 | 通过条件 |
| --- | --- | --- |
| P0 双原型 | 一条真实 Cubism/EGL 离屏渲染链和一条 SCTK layer-shell 链 | 有授权的模型成功加载并渲染动作、遮罩和带 alpha 的帧。私有 Sway、KWin 下用底层测试窗口验证按区域穿透、默认键盘焦点、隐藏后无模型输入，以及 1×、1.5× 缩放和两输出重建。 |
| P1 桌宠可用 | 联通原生 bridge、overlay 和 Rust `terminal` 插件命令，恢复模型列表/切换、动作表情、拖动缩放、显隐与输出选择 | 加载失败保留旧模型。旧帧和旧会话请求被丢弃。IPC 在加载和连续渲染期间可响应。无 Wayland、断连和 SIGTERM 都遵守资源生命周期。对比 `wl_shm` 与 EGL 的帧时间和 IPC 延迟，选择交付呈现路径。 |
| P2 旧版交互与控制面板 | 先验证 GPUI Kit 的 Wayland 最小窗口和输入，再实现面板；恢复裁剪事务、透明度和悬停、模型快捷键及需要迁移的配置，补齐皮肤、口型和外部控制 | 零显示透明度仍按模型轮廓命中，裁剪外穿透，保存失败及取消可恢复，配置迁移覆盖旧字段和坏值。面板通过中文输入与快捷键验收，状态和 CLI 一致。 |
| P3 平台增强与打包 | 分别恢复 KWin 全局视线、Hyprland/Sway 焦点屏、Hyprland 跨屏拖动与指定 GPU | 私有合成器验证可选能力和降级。KWin 实机验证正常退出、异常退出与重连后的脚本清理，实际显卡验证指定 PCI 与跨 GPU。验证无 SDK 基础构建、启用原生功能的安装包及运行依赖。 |

本地验收已覆盖 Sway 的真实宿主与面板、中文输入、跨标签页模型快捷键、配置写入失败回滚，以及 Hyprland 0.55.2 的双屏焦点切换和跨屏拖动预览。私有 KWin 验证了全局指针脚本的正常退出、强制退出和重连清理。Intel DG1 实测指定 PCI 生效，无效 PCI 启动失败后宿主仍可修改选择。DG1 渲染的 Haru 通过 `wl_shm` 交给软件 pixman 合成器后可见，合成器截图对比加载前改变了 20,207 个像素。当前环境只有一个硬件 DRM 渲染节点，双硬件 GPU 的验收仍待执行。

普通 CI 用替身测试命令队列、会话代号、坐标变换、输入区域和状态回滚。带 SDK 的测试分别覆盖渲染像素、资源错误和原生析构。软件 GPU 与私有合成器测试记录其后端和缩放，指定设备和实际交互的结论由对应实机验证提供。 [旧版验证范围](<../docs/需求描述/模型交互与显示.md#L47>) · [当前 Arch CI](<../README.md#L122>)
