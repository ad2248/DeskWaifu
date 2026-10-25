#pragma once

#include <chrono>
#include <string>
#include <systemd/sd-bus.h>

// 由当前进程持有的临时 KWin 脚本，并非安装到系统的 KDE 插件。
class KWinCursorTracker
{
public:
    ~KWinCursorTracker();
    bool Start();
    void Stop();
    void Poll(const std::string& outputName);
    bool LocalPosition(int width, int height, double& x, double& y) const;
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
    std::chrono::steady_clock::time_point _sampleTime{}, _loadedAt{}, _retry{};
};

// 单独提供脚本，以便在没有合成器时也能测试实际脚本。
std::string KWinCursorScript(const std::string& destination, const std::string& scriptName);

// 内部子进程入口，在应用初始化前派发。
int RunKWinCursorGuard(const char* owner, const char* scriptName, const char* client);
