# 日志控制插件

`log_gui_plugin` 提供控制面板中的日志查看页。它负责把宿主和插件产生的日志以可读的 Qt 页面呈现给用户，不参与业务处理，也不改变日志生产者的生命周期。

## 实现原理

`gui/LogGuiPlugin.cpp` 注册 GUI 插件，`LogTab` 负责日志列表、刷新和显示。GUI 插件通过 GUI SDK 订阅或读取宿主提供的日志数据，并在 Qt 界面线程更新模型；它不直接访问 desktop、speech 或其他插件的私有处理器。

日志查看与业务能力解耦：其他插件只需按宿主日志接口输出诊断信息，log 插件即可展示；停止时先解除订阅，再销毁页面，避免日志回调访问已释放控件。

## 构建与验证

`CMakeLists.txt` 以 GUI 模式构建 `log_gui_plugin`，源文件为 `LogGuiPlugin.cpp` 和 `LogTab.cpp`。相关 Qt 页面行为和日志订阅验证应与控制面板构建一起执行。
