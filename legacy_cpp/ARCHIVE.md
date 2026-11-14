# C++ 历史实现

此目录保留根项目切换为 Rust 前的 C++/Qt 实现，恢复来源为提交 `eced97c`。目录名使用 `legacy_cpp`。

包括原宿主、SDK、插件、测试、CMake 配置及配套构建/打包脚本；原 README 保留为历史说明。此目录不参与根目录 Cargo workspace 或当前 CI。

这是源码归档，不是开箱即用的独立发行版：专有 Cubism SDK、未跟踪的 GLEW 依赖和 AUR 子模块未复制，原脚本中的仓库路径、发布入口与 runner 配置也仅反映历史状态。不要用归档内发布脚本发布当前 Rust 版本。

根目录的 Rust 项目与 CI 配置为当前入口。
