#include "DesktopStartup.hpp"
#include "processor/interaction/config/LAppConfig.hpp"
#include "processor/model/LAppDefine.hpp"
#include "contracts/RenderDevice.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

bool ConfigureDesktop(QString* error)
{
    const auto arguments = QCoreApplication::arguments();
    auto& config = LAppConfig::GetInstance();
    std::string path = LAppConfig::GetDefaultConfigPath();
    const int configIndex = arguments.indexOf("--config");
    if (configIndex >= 0 && configIndex + 1 < arguments.size())
        path = LAppConfig::ExpandTilde(arguments[configIndex + 1].toStdString());
    std::string diagnostic;
    if (!config.LoadFromFile(path, &diagnostic) && !diagnostic.empty()) {
        if (error) *error = QString::fromStdString(diagnostic);
        return false;
    }
    std::string device = config.values.renderDevice;
    if (qEnvironmentVariableIsSet("DESKWAIFU_RENDER_DEVICE")) device = qEnvironmentVariable("DESKWAIFU_RENDER_DEVICE").toStdString();
    const int deviceIndex = arguments.indexOf("--render-device");
    if (deviceIndex >= 0) {
        if (deviceIndex + 1 == arguments.size()) {
            if (error) *error = "--render-device requires auto or a PCI address";
            return false;
        }
        device = arguments[deviceIndex + 1].toStdString();
    }
    if (!ValidRenderDevice(device)) {
        if (error) *error = "Invalid render device: expected auto or lowercase PCI address 0000:00:02.0";
        return false;
    }
    if (!device.empty() && device != "auto" && (qEnvironmentVariableIsSet("__EGL_VENDOR_LIBRARY_FILENAMES") ||
        qEnvironmentVariableIsSet("__EGL_VENDOR_LIBRARY_DIRS") || qEnvironmentVariableIsSet("DRI_PRIME"))) {
        if (error) *error = "Unset __EGL_VENDOR_LIBRARY_FILENAMES, __EGL_VENDOR_LIBRARY_DIRS and DRI_PRIME";
        return false;
    }
    qputenv("DESKWAIFU_RENDER_DEVICE", QByteArray::fromStdString(device));
    QString models = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/deskwaifu/models";
    const int modelsIndex = arguments.indexOf("--models_dir");
    if (modelsIndex >= 0 && modelsIndex + 1 < arguments.size()) models = arguments[modelsIndex + 1];
    LAppDefine::ModelsDir = QDir(models).absolutePath().toStdString() + "/";
    LAppDefine::RenderTargetWidth = config.values.windowWidth;
    LAppDefine::RenderTargetHeight = config.values.windowHeight;
    return true;
}
