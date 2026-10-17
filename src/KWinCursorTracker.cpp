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
    input.imbue(std::locale::classic()); // JavaScript always serializes decimal points as '.'.
    input >> std::noskipws >> value;
    return !input.fail() && input.eof() && std::isfinite(value);
}
}

int RunKWinCursorGuard(const char* owner, const char* scriptName, const char* client)
{
    // Detach so the cleaner can outlive a SIGKILL of the renderer. The short-
    // lived intermediate is reaped by StartGuard; init reaps the actual cleaner.
    const pid_t child = fork();
    if (child < 0) return 1;
    if (child > 0) return 0;
    // Survive terminal group signals; do not match the renderer's killall
    // toggle/focus commands when reusing the same executable.
    if (setsid() < 0 || prctl(PR_SET_NAME, "dw-kwin-clean", 0, 0, 0) < 0) _exit(1);
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) _exit(1);
    sd_bus_set_method_call_timeout(bus, 250000);
    if (write(STDOUT_FILENO, "R", 1) != 1) _exit(1);
    pollfd control{STDIN_FILENO, POLLIN, 0};
    int result;
    do { result = poll(&control, 1, -1); } while (result < 0 && errno == EINTR);
    // EOF on the control pipe is independent of whether the script has run.
    auto hasOwner = [bus](const char* name) {
        sd_bus_message* reply = nullptr;
        int alive = 1;
        int result = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner", nullptr, &reply, "s", name);
        if (result >= 0) result = sd_bus_message_read(reply, "b", &alive);
        sd_bus_message_unref(reply);
        return result < 0 ? result : alive;
    };
    // Wait until the broker has drained the dead client's messages. Otherwise
    // cleanup from this connection could overtake a still-queued loadScript.
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
        usleep(200000); // Keep cleaning after transient compositor timeouts.
    }
    sd_bus_flush_close_unref(bus);
    _exit(0);
}

std::string KWinCursorScript(const std::string& destination, const std::string& scriptName)
{
    // Both strings are generated from the D-Bus unique name, never user input.
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
        // JS numeric variants can become either D-Bus int32 or double. Explicit
        // string arguments keep the wire signature stable for both kinds.
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
    // A timeout does not cancel a server-side registration. Clean it up before
    // reusing the name, and let KWin finish its deferred deletion before retrying.
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
    if (!StartGuard()) return false; // Must be ready before any registration request.

    // KWin opens this path when run() reads the script. No file is installed,
    // and the anonymous file disappears automatically if this process dies.
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
    if (result >= 0 && id < 0) _registered = false; // Explicit rejection, not a lost reply.
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
        return false; // Still uncertain: do not overwrite the registration on retry.
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
    // Do not keep the renderer's Wayland, IPC, GPU or control-write FDs alive.
    if (result == 0) result = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
    const char* client = nullptr;
    sd_bus_get_unique_name(_bus, &client);
    char* args[] = {const_cast<char*>("/proc/self/exe"), const_cast<char*>("--kwin-cursor-guard"),
        const_cast<char*>(_owner.c_str()), const_cast<char*>(_scriptName.c_str()),
        const_cast<char*>(client), nullptr};
    pid_t child = -1;
    if (result == 0) result = posix_spawn(&child, "/proc/self/exe", &actions, nullptr, args, environ);
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
        // Usually immediate; never make shutdown wait indefinitely for KWin.
        // A cleaner that is still retrying remains independent after we return.
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
    // Bound dispatch work so a busy session bus cannot starve rendering.
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
        // An acquisition signal queued before GetNameOwner must not discard
        // registration state for the very same KWin connection.
        if (self._owner != newOwner) {
            self._loaded = false;
            self._registered = false;
            self._sampleTime = {};
            self._retry = {};
        }
    }
    return 0;
}
