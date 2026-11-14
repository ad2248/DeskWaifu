# 模型控制插件

`model_gui_plugin` 是控制面板中的模型页，提供模型选择和模型动作控制。它是 GUI 适配层，不直接创建或操作 Cubism 模型、纹理和渲染线程。

## 功能

- 浏览和切换可用模型；
- 触发表情、动作和快捷键；
- 切换皮肤及其他模型显示选项；
- 查询当前模型、动作和表情列表。

## 实现原理

`gui/ModelGuiPlugin.cpp` 注册 GUI 插件，`ModelTab` 负责 Qt 页面、用户输入和命令结果。页面把操作转换为 desktop 提供的 `command.set_model/v1`、`next_model/v1`、`prev_model/v1`、`trigger_hotkey/v1`、`set_expression/v1`、`do_motion/v1` 和 `switch_skin/v1` 等命令。

模型的实际生命周期、资源加载、动作执行、纹理管理和渲染线程归 `desktop` 插件所有。模型控制页只保存界面状态并等待异步命令结果，因此不会跨线程借用模型对象，也不会绕过 desktop 的命令串行化和取消规则。

## 构建与验证

`CMakeLists.txt` 以 GUI 模式生成 `model_gui_plugin`。模型领域的完整契约、命中测试、动作和资源生命周期测试位于 `plugins/desktop/tests/`；控制面板代码通过对应 Qt GUI 测试和桌面命令测试验证。
