#pragma once

#include "MainWindow.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>

// The theme checks exercise installed-style dynamic pages, not a second static
// copy of their UI. Give the real shell a private, disconnected IPC location.
class ControlTestWindow : public MainWindow
{
public:
    ControlTestWindow() : MainWindow(nullptr, QStringLiteral("/nonexistent-deskwaifu-proc")) {}
};

// Named widget view retained for the existing visual regression assertions.
// It only looks up pages loaded by the production plugin manager.
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
    X(QStatusBar, statusBar) \
    X(QPlainTextEdit, responseLog)
#define DECLARE_WIDGET(Type, name) Type* name = nullptr;
    CONTROL_TEST_WIDGETS(DECLARE_WIDGET)
#undef DECLARE_WIDGET
    void setupUi(MainWindow* window)
    {
#define FIND_WIDGET(Type, name) name = window->findChild<Type*>(QStringLiteral(#name)); Q_ASSERT(name);
        CONTROL_TEST_WIDGETS(FIND_WIDGET)
#undef FIND_WIDGET
    }
#undef CONTROL_TEST_WIDGETS
};
