#include "KWinCursorTracker.hpp"

#include <cmath>
#include <cstring>
#include <locale>
#include <sstream>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>

extern char** environ;

namespace {
constexpr const char* Path = "/org/deskwaifu/Cursor";
constexpr const char* Interface = "org.deskwaifu.Cursor";
constexpr const char* KWin = "org.kde.KWin";
constexpr const char* Scripting = "org.kde.kwin.Scripting";

bool Number(const char* text, double& value)
{
    if (!text || !*text || std::strlen(text) > 64) return false;
    std::istringstream input(text);
    input.imbue(std::locale::classic()); // JavaScript 序列化时始终使用 '.' 作为小数点。
    input >> std::noskipws >> value;
    return !input.fail() && input.eof() && std::isfinite(value);
}
}

int RunKWinCursorGuard(const char* owner, const char* scriptName, const char* client)
{
    // 分离清理进程，使渲染进程被 SIGKILL 后它仍能存活。
    // 短暂存在的中间进程由 StartGuard 回收，实际清理进程由 init 回收。
    const pid_t child = fork();
    if (child < 0) return 1;
    if (child > 0) return 0;
    // 避免被终端进程组信号终止；复用同一可执行文件时，不能被渲染进程的
    // killall 显示切换或焦点命令匹配到。
    if (setsid() < 0 || prctl(PR_SET_NAME, "dw-kwin-clean", 0, 0, 0) < 0) _exit(1);
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) _exit(1);
    sd_bus_set_method_call_timeout(bus, 250000);
    if (write(STDOUT_FILENO, "R", 1) != 1) _exit(1);
    pollfd control{STDIN_FILENO, POLLIN, 0};
    int result;
    do { result = poll(&control, 1, -1); } while (result < 0 && errno == EINTR);
    // 控制管道是否到达 EOF，与脚本是否已运行无关。
    auto hasOwner = [bus](const char* name) {
        sd_bus_message* reply = nullptr;
        int alive = 1;
        int result = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner", nullptr, &reply, "s", name);
        if (result >= 0) result = sd_bus_message_read(reply, "b", &alive);
        sd_bus_message_unref(reply);
        return result < 0 ? result : alive;
    };
    // 等待消息代理处理完已退出客户端的消息，否则当前连接的清理操作
    // 可能早于仍在排队的 loadScript 执行。
    while (hasOwner(client) != 0) {
        if (hasOwner(owner) == 0 || !sd_bus_is_open(bus)) _exit(0);
        usleep(200000);
    }
    for (;;) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        result = sd_bus_call_method(bus, owner, "/Scripting", Scripting,
            "unloadScript", &error, nullptr, "s", scriptName);
        const bool gone = sd_bus_error_has_name(&error, SD_BUS_ERROR_SERVICE_UNKNOWN) ||
            sd_bus_error_has_name(&error, SD_BUS_ERROR_NAME_HAS_NO_OWNER);
        sd_bus_error_free(&error);
        if (result >= 0 || gone || !sd_bus_is_open(bus)) break;
        usleep(200000); // 合成器暂时超时后继续清理。
    }
    sd_bus_flush_close_unref(bus);
    _exit(0);
}

std::string KWinCursorScript(const std::string& destination, const std::string& scriptName)
{
    // 两个字符串都由 D-Bus 唯一名称生成，不来自用户输入。
    return "var destination = '" + destination + "';\nvar scriptName = '" + scriptName + R"JS(';
var outputName = '';
var pending = false;
var sampleTimer = new QTimer();
sampleTimer.interval = 30;
sampleTimer.timeout.connect(function () {
    if (pending) return;
    var rect = {x: 0, y: 0, width: 0, height: 0};
    var name = '';
    var screens = workspace.screens;
    for (var i = 0; i < screens.length; ++i) {
        if (screens[i].name === outputName) {
            rect = screens[i].geometry;
            name = outputName;
            break;
        }
    }
    var pos = workspace.cursorPos;
    pending = true;
    callDBus(destination, '/org/deskwaifu/Cursor', 'org.deskwaifu.Cursor', 'Sample',
        String(pos.x), String(pos.y), name, String(rect.x), String(rect.y),
        String(rect.width), String(rect.height), function (requestedOutput) {
            outputName = requestedOutput;
            pending = false;
        });
});
sampleTimer.start();

// The unique bus name disappears even after SIGKILL. Clean up inside KWin too,
// rather than relying solely on the application's normal shutdown handler.
var watchdog = new QTimer();
watchdog.interval = 2000;
watchdog.timeout.connect(function () {
    callDBus('org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'NameHasOwner', destination, function (alive) {
            if (alive) return;
            sampleTimer.stop();
            watchdog.stop();
            callDBus('org.kde.KWin', '/Scripting', 'org.kde.kwin.Scripting',
                'unloadScript', scriptName);
        });
});
watchdog.start();
)JS";
}

KWinCursorTracker::~KWinCursorTracker() { Stop(); }

bool KWinCursorTracker::Start()
{
    if (_bus) return _loaded;
    int result = sd_bus_open_user(&_bus);
    if (result < 0) {
        _error = std::strerror(-result);
        return false;
    }
    sd_bus_set_method_call_timeout(_bus, 250000);
    const char* unique = nullptr;
    sd_bus_get_unique_name(_bus, &unique);
    _scriptName = std::string("deskwaifu-cursor-") + unique + "-" + std::to_string(getpid());
    static const sd_bus_vtable table[] = {
        SD_BUS_VTABLE_START(0),
        // JS 数值可能转换为 D-Bus int32 或 double。
        // 显式使用字符串参数，确保两种情况下的协议签名一致。
        SD_BUS_METHOD("Sample", "sssssss", "s", Sample, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END
    };
    result = sd_bus_add_object_vtable(_bus, &_object, Path, Interface, table, this);
    if (result >= 0)
        result = sd_bus_match_signal(_bus, &_watch, "org.freedesktop.DBus",
            "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged", OwnerChanged, this);
    if (result < 0) {
        _error = std::strerror(-result);
        Stop();
        return false;
    }
    return Load();
}

bool KWinCursorTracker::Load()
{
    _retry = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    _sampleTime = {};
    // 超时不会取消服务端注册。复用名称前先清理注册，
    // 重试前等待 KWin 完成延迟删除。
    if (_registered) {
        Unload();
        return false;
    }
    sd_bus_message* reply = nullptr;
    int result = sd_bus_call_method(_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", nullptr, &reply, "s", KWin);
    const char* owner = nullptr;
    if (result >= 0) result = sd_bus_message_read(reply, "s", &owner);
    if (result >= 0) _owner = owner;
    sd_bus_message_unref(reply);
    if (result < 0) {
        _error = "KWin unavailable: " + std::string(std::strerror(-result));
        return false;
    }
    if (!StartGuard()) return false; // 必须在发送任何注册请求前就绪。

    // KWin 在 run() 读取脚本时打开此路径。不安装任何文件，
    // 进程退出后匿名文件会自动消失。
    if (_scriptFd >= 0) close(_scriptFd);
    _scriptFd = memfd_create("deskwaifu-kwin-cursor", MFD_CLOEXEC);
    const char* unique = nullptr;
    sd_bus_get_unique_name(_bus, &unique);
    const auto script = KWinCursorScript(unique, _scriptName);
    if (_scriptFd < 0 || write(_scriptFd, script.data(), script.size()) != static_cast<ssize_t>(script.size())) {
        _error = "Cannot create transient KWin script";
        return false;
    }
    const auto file = "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(_scriptFd);
    reply = nullptr;
    _registered = true;
    result = sd_bus_call_method(_bus, _owner.c_str(), "/Scripting", Scripting,
        "loadScript", nullptr, &reply, "ss", file.c_str(), _scriptName.c_str());
    int id = -1;
    if (result >= 0) result = sd_bus_message_read(reply, "i", &id);
    sd_bus_message_unref(reply);
    if (result >= 0 && id < 0) _registered = false; // 明确拒绝，而非回复丢失。
    if (result < 0 || id < 0) {
        _error = "KWin could not load transient cursor script";
        return false;
    }
    const auto path = "/Scripting/Script" + std::to_string(id);
    result = sd_bus_call_method(_bus, _owner.c_str(), path.c_str(), "org.kde.kwin.Script",
        "run", nullptr, nullptr, "");
    if (result < 0) {
        Unload();
        _error = "KWin could not run transient cursor script";
        return false;
    }
    _loaded = true;
    _loadedAt = std::chrono::steady_clock::now();
    _error.clear();
    return true;
}

bool KWinCursorTracker::Unload()
{
    _loaded = false;
    _sampleTime = {};
    if (!_registered) return true;
    const int result = sd_bus_call_method(_bus, _owner.c_str(), "/Scripting", Scripting,
        "unloadScript", nullptr, nullptr, "s", _scriptName.c_str());
    if (result < 0) {
        _error = "KWin could not unload transient cursor script; retrying";
        return false; // 结果仍不确定，重试时不能覆盖已有注册。
    }
    _registered = false;
    return true;
}

void KWinCursorTracker::Stop()
{
    if (_bus) Unload();
    _registered = false;
    if (_scriptFd >= 0) close(_scriptFd);
    _scriptFd = -1;
    _watch = sd_bus_slot_unref(_watch);
    _object = sd_bus_slot_unref(_object);
    _bus = sd_bus_flush_close_unref(_bus);
    StopGuard();
}

bool KWinCursorTracker::StartGuard()
{
    if (_guardControl >= 0 && _guardOwner == _owner) return true;
    StopGuard();
    int control[2], status[2];
    if (pipe2(control, O_CLOEXEC) < 0) {
        _error = "Cannot create KWin cleanup control pipe";
        return false;
    }
    if (pipe2(status, O_CLOEXEC) < 0) {
        close(control[0]); close(control[1]);
        _error = "Cannot create KWin cleanup status pipe";
        return false;
    }
    posix_spawn_file_actions_t actions;
    const int initialized = posix_spawn_file_actions_init(&actions);
    int result = initialized;
    if (result == 0) result = posix_spawn_file_actions_adddup2(&actions, control[0], STDIN_FILENO);
    if (result == 0) result = posix_spawn_file_actions_adddup2(&actions, status[1], STDOUT_FILENO);
    // 不保留渲染进程的 Wayland、IPC、GPU 或控制管道写端文件描述符。
    if (result == 0) result = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
    const char* client = nullptr;
    sd_bus_get_unique_name(_bus, &client);
    char executable[PATH_MAX];
    const auto length = readlink("/proc/self/exe", executable, sizeof(executable));
    std::string helper;
    if (length > 0 && length < static_cast<ssize_t>(sizeof(executable))) {
        helper.assign(executable, length);
        helper.resize(helper.rfind('/') + 1);
        helper += "deskwaifu-cursor-guard";
    } else result = EINVAL;
    char* args[] = {const_cast<char*>(helper.c_str()),
        const_cast<char*>(_owner.c_str()), const_cast<char*>(_scriptName.c_str()),
        const_cast<char*>(client), nullptr};
    pid_t child = -1;
    if (result == 0) result = posix_spawn(&child, helper.c_str(), &actions, nullptr, args, environ);
    if (initialized == 0) posix_spawn_file_actions_destroy(&actions);
    close(control[0]); close(status[1]);
    if (result == 0) {
        int childStatus = 0;
        while (waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {}
        pollfd ready{status[0], POLLIN, 0};
        do { result = poll(&ready, 1, 1000); } while (result < 0 && errno == EINTR);
        char ack = 0;
        if (result > 0 && read(status[0], &ack, 1) == 1 && ack == 'R') {
            _guardControl = control[1];
            _guardStatus = status[0];
            _guardOwner = _owner;
            return true;
        }
    }
    close(control[1]); close(status[0]);
    _error = "Cannot start independent KWin script cleanup";
    return false;
}

void KWinCursorTracker::StopGuard()
{
    if (_guardControl >= 0) close(_guardControl);
    _guardControl = -1;
    if (_guardStatus >= 0) {
        // 通常会立即完成，退出时不能无限等待 KWin。
        // 返回后，仍在重试的清理进程继续独立运行。
        pollfd finished{_guardStatus, POLLIN, 0};
        int result;
        do { result = poll(&finished, 1, 500); } while (result < 0 && errno == EINTR);
        close(_guardStatus);
    }
    _guardStatus = -1;
    _guardOwner.clear();
}

void KWinCursorTracker::Poll(const std::string& outputName)
{
    _outputName = outputName;
    if (!_bus) return;
    // 限制每次派发的工作量，避免繁忙的会话总线阻塞渲染。
    for (int i = 0; i < 32 && sd_bus_process(_bus, nullptr) > 0; ++i) {}
    const auto lastSample = _sampleTime == std::chrono::steady_clock::time_point{} ? _loadedAt : _sampleTime;
    if (_loaded && std::chrono::steady_clock::now() - lastSample > std::chrono::seconds(3)) {
        Unload();
        _retry = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        _error = "KWin cursor script stopped responding; retrying";
    }
    if (!_loaded && std::chrono::steady_clock::now() >= _retry) Load();
}

bool KWinCursorTracker::LocalPosition(int width, int height, double& x, double& y) const
{
    if (!_loaded || _sampleOutput != _outputName || _width <= 0 || _height <= 0 ||
        width <= 0 || height <= 0 ||
        std::chrono::steady_clock::now() - _sampleTime > std::chrono::seconds(1)) return false;
    x = (_x - _left) * width / _width;
    y = (_y - _top) * height / _height;
    return true;
}

int KWinCursorTracker::Sample(sd_bus_message* message, void* data, sd_bus_error*)
{
    auto& self = *static_cast<KWinCursorTracker*>(data);
    const char* sender = sd_bus_message_get_sender(message);
    if (!sender || self._owner != sender)
        return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_ACCESS_DENIED, "Only KWin may send cursor samples");
    double x, y, left, top, width, height;
    const char *name, *sx, *sy, *sl, *st, *sw, *sh;
    const int result = sd_bus_message_read(message, "sssssss", &sx, &sy, &name, &sl, &st, &sw, &sh);
    if (result < 0) return result;
    if (!Number(sx, x) || !Number(sy, y) || !Number(sl, left) || !Number(st, top) ||
        !Number(sw, width) || !Number(sh, height) || width < 0 || height < 0)
        return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_INVALID_ARGS, "Invalid cursor geometry");
    self._x = x; self._y = y; self._left = left; self._top = top;
    self._width = width; self._height = height; self._sampleOutput = name;
    self._sampleTime = std::chrono::steady_clock::now();
    if (self._scriptFd >= 0) close(self._scriptFd);
    self._scriptFd = -1;
    return sd_bus_reply_method_return(message, "s", self._outputName.c_str());
}

int KWinCursorTracker::OwnerChanged(sd_bus_message* message, void* data, sd_bus_error*)
{
    const char *name, *oldOwner, *newOwner;
    const int result = sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner);
    if (result < 0) return result;
    if (std::strcmp(name, KWin) == 0) {
        auto& self = *static_cast<KWinCursorTracker*>(data);
        // GetNameOwner 之前排队的名称获取信号，不能清除同一 KWin 连接的注册状态。
        if (self._owner != newOwner) {
            self._loaded = false;
            self._registered = false;
            self._sampleTime = {};
            self._retry = {};
        }
    }
    return 0;
}
