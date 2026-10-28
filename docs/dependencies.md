# 下载构建依赖

## 前提

- 当前构建固定使用 Linux x86_64 的 Cubism Core 静态库，不会自动选择其他架构的库。
- 先按 [README 的前置依赖说明](../README.md#prerequisites)准备默认完整构建的系统开发依赖：CMake ≥ 3.21、支持 Qt 6 的 C++ 工具链、Bash、pkg-config、OpenGL/EGL、libsystemd、Qt ≥ 6.5 和 LayerShellQt ≥ 6.6。关闭控制面板本身不能免除 Qt；启用 perception 时截图仍需 DBus、后台图像编码仍需 Concurrent。默认启用的语音插件另需 Multimedia/WebSockets；设置 `-DDESKWAIFU_BUILD_SPEECH_PLUGIN=OFF` 可省去这两项，但不能用于完整门禁。桌面专属依赖可随 `DESKWAIFU_BUILD_DESKTOP_PLUGIN=OFF` 关闭，其他后端也有独立构建开关；见[插件构建说明](plugin-architecture.md#构建与开发)。
- 默认构建测试，还需 `wayland-scanner`、Wayland 开发库及（控制面板开启时的）Qt Test。仅构建应用可设置 `-DBUILD_TESTING=OFF`；完整测试门禁另需 Python 3、Node.js 和 `dbus-run-session`。

## 下载与解压

在仓库根目录执行，需安装 `curl`、`unzip`、`tar`（支持 gzip 解压）。仓库中的 `thirdParty/` 应已存在，解压产生的 `Cubism/`、`thirdParty/glew-2.3.1/` 及两个最终目标目录均须尚不存在。

以下为镜像下载示例：**外部地址的可达性、归档内容、实际版本和校验和均未验证**。命令假定 ZIP 顶层目录为 `Cubism/`、TGZ 顶层目录为 `glew-2.3.1/`；执行前应确认来源可信，下载后核对内容，重命名不代表版本验证。也可按 [README](../README.md#building-from-source) 从官方来源准备依赖。使用 Cubism SDK 须遵守其附带的许可条款。

```bash
curl -fL https://cdn.kals.top/CubismFull.zip -o CubismFull.zip
unzip -q CubismFull.zip
mv Cubism CubismSdkForNative-5-r.5

curl -fL https://cdn.kals.top/glew-2.3.1.tgz -o glew-2.3.1.tgz
tar -xzf glew-2.3.1.tgz -C thirdParty
mv thirdParty/glew-2.3.1 thirdParty/glew
```

## 目录检查与构建

默认使用 Cubism SDK for Native **5-r.5** 和 GLEW **2.3.1**，不能仅凭目录存在判断依赖完整：

- `CubismSdkForNative-5-r.5/`：需包含 `Core/include/`、`Core/lib/linux/x86_64/libLive2DCubismCore.a`、`Framework/`、`Samples/Common/`，以及 `Framework/src/Rendering/OpenGL/Shaders/Standard/` 和 `Samples/OpenGL/Shaders/Standard/` 下的着色器。
- `thirdParty/glew/`：需为完整源码发行包，包含 `build/cmake/CMakeLists.txt`、`config/version`、`include/GL/glew.h` 和 `src/glew.c`；系统 GLEW 开发包不能直接替代此目录。

准备完成后，**跳过 README 手动构建步骤中的 `setup_glew`**，避免重复下载及将版本目录嵌套到现有 `glew/`。在仓库根目录直接构建应用（不构建测试）：

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --parallel
```

如需构建测试，改为 `-DBUILD_TESTING=ON` 并补齐上述测试依赖。也可使用 README 推荐的 `install.sh`：它会跳过已有 GLEW 头文件的下载，但该检查不验证版本或目录完整性，且默认仍构建测试。

开发运行还需让主程序配置的 `plugin_dir` 指向构建目录的 `plugins/`；控制面板可用 `DESKWAIFU_CONTROL_PLUGIN_DIR` 指向同一目录。默认插件路径是安装前缀下的目录，仅构建而不安装不会自动切换该路径。详见 [插件构建说明](plugin-architecture.md#构建与开发) 和 [GUI 插件运行说明](gui-plugins.md#构建与运行)。
