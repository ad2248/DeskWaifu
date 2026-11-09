#pragma once

#include "MainWindow.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>

// The theme checks exercise installed-style dynamic pages, not a second static
// copy of their UI. Give the real shell a private, disconnected IPC location.
// 使用不存在的进程目录创建真实控制外壳，避免测试连接运行中的后端。
// 页面仍由生产插件管理器动态加载，须在 GUI 主线程使用。
class ControlTestWindow : public MainWindow
{
public:
    ControlTestWindow() : MainWindow(nullptr, QStringLiteral("/nonexistent-deskwaifu-proc")) {}
};

// Named widget view retained for the existing visual regression assertions.
// It only looks up pages loaded by the production plugin manager.
// 保存真实插件页面的非拥有控件指针；所属窗口及页面必须在断言期间保持有效。
struct ControlTestWidgets
{
#define CONTROL_TEST_WIDGETS(X) \
    X(QTabWidget, tabWidget) \
    X(QScrollArea, systemTab) \
    X(QScrollArea, interactionTab) \
    X(QPushButton, spawnButton) \
    X(QPushButton, respawnButton) \
    X(QComboBox, renderDeviceCombo) \
    X(QComboBox, modelCombo) \
    X(QGroupBox, renderDeviceGroup) \
    X(QGroupBox, hotkeyGroup) \
    X(QGroupBox, hoverOpacityGroup) \
    X(QCheckBox, hoverOpacityEnabledCheck) \
    X(QSlider, hoverOpacitySlider) \
    X(QSlider, idleOpacitySlider) \
    X(QSpinBox, cropBox_x) \
    X(QSpinBox, cropBox_y) \
    X(QSpinBox, cropBox_w) \
    X(QSpinBox, cropBox_h) \
    X(QLabel, currentModelLabel) \
    X(QStatusBar, statusBar)
#define DECLARE_WIDGET(Type, name) Type* name = nullptr;
    CONTROL_TEST_WIDGETS(DECLARE_WIDGET)
#undef DECLARE_WIDGET
    // 按对象名称查找必需控件，不创建 UI；缺失控件触发断言。
    void setupUi(MainWindow* window)
    {
#define FIND_WIDGET(Type, name) name = window->findChild<Type*>(QStringLiteral(#name)); Q_ASSERT(name);
        CONTROL_TEST_WIDGETS(FIND_WIDGET)
#undef FIND_WIDGET
    }
#undef CONTROL_TEST_WIDGETS
};
