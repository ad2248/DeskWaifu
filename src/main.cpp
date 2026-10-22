/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
#include "LAppDefine.hpp"
#include "LAppConfig.hpp"
#include "RenderDevice.hpp"
#include <cstdlib>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <cstring>
#include <string>
#include <climits>
#include <cstdio>
#include <QGuiApplication>

int main(int argc, char* argv[])
{
    if (argc == 5 && std::strcmp(argv[1], "--kwin-cursor-guard") == 0)
        return RunKWinCursorGuard(argv[2], argv[3], argv[4]);
    // deskwaifu toggle
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "toggle") == 0 || strcmp(argv[i], "--toggle") == 0) {
            system("killall -s SIGUSR1 deskwaifu");
            return 0;
        }
        if (strcmp(argv[i], "focus") == 0 || strcmp(argv[i], "--focus") == 0) {
            system("killall -s SIGUSR2 deskwaifu");
            return 0;
        }
    }

    // Parse CLI args
    std::string modelsDir = "";
    std::string configPath = "";
    std::string renderDevice;
    bool renderDeviceOverride = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--render-device") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "[GPU] --render-device requires auto or a PCI address (0000:00:02.0)\n");
                return 1;
            }
            renderDevice = argv[++i];
            renderDeviceOverride = true;
            continue;
        }
        if (strcmp(argv[i], "--models_dir") == 0 && i + 1 < argc) {
            modelsDir = argv[i + 1];
            i++;
        }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[i + 1];
            i++;
        }
    }

    // Load config
    LAppConfig& config = LAppConfig::GetInstance();
    if (configPath.empty()) {
        configPath = LAppConfig::GetDefaultConfigPath();
    } else {
        configPath = LAppConfig::ExpandTilde(configPath);
    }
    std::string configError;
    if (config.LoadFromFile(configPath, &configError)) {
        printf("[APP]Loaded config from: %s\n", configPath.c_str());
    } else if (!configError.empty()) {
        fprintf(stderr, "[Config] Invalid configuration at %s: %s\n", configPath.c_str(), configError.c_str());
        return 1;
    } else {
        printf("[APP]Config unavailable or invalid at: %s (using defaults)\n", configPath.c_str());
    }

    // Select only the offscreen renderer; leave Qt's Wayland GPU untouched.
    if (!renderDeviceOverride) {
        const char* environment = getenv("DESKWAIFU_RENDER_DEVICE");
        renderDevice = environment ? environment : config.values.renderDevice;
    }
    if (!ValidRenderDevice(renderDevice)) {
        fprintf(stderr, "[GPU] Invalid render device '%s': expected auto or lowercase PCI address 0000:00:02.0\n", renderDevice.c_str());
        return 1;
    }
    if (!renderDevice.empty() && renderDevice != "auto" &&
        (getenv("__EGL_VENDOR_LIBRARY_FILENAMES") || getenv("__EGL_VENDOR_LIBRARY_DIRS") || getenv("DRI_PRIME"))) {
        fprintf(stderr, "[GPU] Unset __EGL_VENDOR_LIBRARY_FILENAMES, __EGL_VENDOR_LIBRARY_DIRS and DRI_PRIME; --render-device selects only the offscreen GPU\n");
        return 1;
    }
    qputenv("DESKWAIFU_RENDER_DEVICE", QByteArray::fromStdString(renderDevice));

    // Models dir: CLI flag > default XDG path
    if (modelsDir.empty()) {
        const char *configHome = getenv("XDG_CONFIG_HOME");
        if (configHome && strlen(configHome) > 0) {
            modelsDir = std::string(configHome) + "/deskwaifu/models/";
        } else {
            const char *homeDir = getenv("HOME");
            if (!homeDir || strlen(homeDir) == 0) {
                homeDir = getpwuid(getuid())->pw_dir;
            }
            modelsDir = std::string(homeDir) + "/.config/deskwaifu/models/";
        }
    }

    // Resolve absolute path
    char resolved_path[PATH_MAX];
    if (realpath(modelsDir.c_str(), resolved_path) != NULL) {
        modelsDir = std::string(resolved_path);
    }

    // Ensure modelsDir ends with trailing slash
    if (!modelsDir.empty() && modelsDir.back() != '/') {
        modelsDir += "/";
    }

    LAppDefine::ModelsDir = modelsDir;

    // Apply config to LAppDefine
    LAppDefine::RenderTargetWidth = config.values.windowWidth;
    LAppDefine::RenderTargetHeight = config.values.windowHeight;

    // The guard subprocess above intentionally does not initialize Qt or Wayland.
    QGuiApplication application(argc, argv);
    application.setApplicationName("deskwaifu");
    application.setQuitOnLastWindowClosed(false);
    // create the application instance
    if (LAppDelegate::GetInstance()->Initialize() == GL_FALSE)
    {
        if (!LAppDelegate::GetInstance()->Release())
            fprintf(stderr, "[APP]Failed to bind GPU context for teardown\n");
        LAppDelegate::ReleaseInstance();
        return 1;
    }

    return LAppDelegate::GetInstance()->Run() ? 0 : 1;
}
