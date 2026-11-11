#pragma once

#include <chrono>
#include <string>
#include <systemd/sd-bus.h>

// 由当前进程持有的临时 KWin 脚本，并非安装到系统的 KDE 插件。
class KWinCursorTracker {
public:
    // 析构时停止追踪并释放总线、匿名脚本文件与清理管道。
    ~KWinCursorTracker();
    // 连接会话总线并加载脚本，成功返回 true；后续恢复由 Poll() 驱动。
    // 所有操作须由同一 Qt 应用线程串行调用。
    bool Start();
    // 请求卸载脚本；独立清理进程可在本进程退出后继续完成清理。
    void Stop();
    // 有界派发总线消息，更新目标输出名称，并重试失联的脚本。
    void Poll(const std::string& outputName);
    // 将新鲜且匹配目标输出的采样映射到指定窗口尺寸；失败不改写 x、y。
    bool LocalPosition(int width, int height, double& x, double& y) const;
    bool Fullscreen() const { return _fullscreen; }
    bool FullscreenChanged() const { return _fullscreenChanged; }
    void AcknowledgeFullscreen() { _fullscreenChanged = false; }
    const std::string& Error() const { return _error; }
    const std::string& ScriptName() const { return _scriptName; }

    KWinCursorTracker() = default;
    KWinCursorTracker(const KWinCursorTracker&) = delete;
    KWinCursorTracker& operator=(const KWinCursorTracker&) = delete;

private:
    bool Load();
    bool Unload();
    bool StartGuard();
    void StopGuard();
    static int Sample(sd_bus_message*, void*, sd_bus_error*);
    static int OwnerChanged(sd_bus_message*, void*, sd_bus_error*);
    sd_bus* _bus = nullptr;
    sd_bus_slot* _object = nullptr;
    sd_bus_slot* _watch = nullptr;
    int _scriptFd = -1;
    int _guardControl = -1, _guardStatus = -1;
    std::string _guardOwner;
    bool _loaded = false;
    bool _registered = false; // 也包括已发送但回复超时的 loadScript 请求。
    std::string _owner, _scriptName, _outputName, _sampleOutput, _error;
    double _x = 0, _y = 0, _left = 0, _top = 0, _width = 0, _height = 0;
    bool _fullscreen = false, _fullscreenChanged = false;
    std::chrono::steady_clock::time_point _sampleTime { }, _loadedAt { }, _retry { };
};

// 单独提供脚本，以便在没有合成器时也能测试实际脚本。
std::string KWinCursorScript(const std::string& destination, const std::string& scriptName);

// 内部子进程入口，在应用初始化前派发。
int RunKWinCursorGuard(const char* owner, const char* scriptName, const char* client);
