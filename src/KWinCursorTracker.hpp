#pragma once

#include <chrono>
#include <string>
#include <systemd/sd-bus.h>

// A transient KWin script owned by this process, not an installed KDE plugin.
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
    static int Sample(sd_bus_message*, void*, sd_bus_error*);
    static int OwnerChanged(sd_bus_message*, void*, sd_bus_error*);
    sd_bus* _bus = nullptr;
    sd_bus_slot* _object = nullptr;
    sd_bus_slot* _watch = nullptr;
    int _scriptFd = -1;
    bool _loaded = false;
    std::string _owner, _scriptName, _outputName, _sampleOutput, _error;
    double _x = 0, _y = 0, _left = 0, _top = 0, _width = 0, _height = 0;
    std::chrono::steady_clock::time_point _sampleTime{}, _loadedAt{}, _retry{};
};

// Kept separate so the actual script can also be exercised without a compositor.
std::string KWinCursorScript(const std::string& destination, const std::string& scriptName);
