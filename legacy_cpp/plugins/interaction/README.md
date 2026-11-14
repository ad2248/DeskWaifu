# 交互控制插件

`interaction_gui_plugin` 是控制面板中的交互设置页，负责展示和编辑桌面交互相关配置，不直接拥有桌面窗口或模型运行时。

## 功能

- 配置鼠标注视、悬停透明度和输出屏幕；
- 编辑模型位置、缩放、裁剪等变换设置；
- 展示控制状态、保存结果和可用输出；
- 管理快捷键面板，绑定或触发桌面交互动作。

## 实现原理

`gui/InteractionGuiPlugin.cpp` 注册 GUI 插件，`InteractionTab` 负责 Qt 页面和控件状态，`HotkeyPanel` 负责快捷键展示与编辑。页面通过 GUI SDK 的命令/事件接口调用 `desktop` 提供的 `command.* /v1` 能力，例如 `get_mouse_look`、`set_mouse_look`、`begin_transform_edit` 和 `apply_transform_edit`。

变换编辑遵循开始、更新、应用/取消的事务流程；界面只维护编辑态和回复状态，真正的窗口、输入和模型变换由 desktop 插件的统一所有者执行。快捷键面板同样不直接访问模型线程，动作通过命令能力投递。

## 构建与验证

插件由 `CMakeLists.txt` 以 GUI 模式构建，包含 `InteractionGuiPlugin.cpp`、`InteractionTab.cpp` 和 `HotkeyPanel.cpp`。`tests/` 中的 `HotkeyPanelTest` 验证快捷键面板行为；桌面侧的交互命令和变换事务测试位于 `plugins/desktop/tests/`。
