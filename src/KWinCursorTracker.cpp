#include "KWinCursorTracker.hpp"

#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr const char* Path = "/org/deskwaifu/Cursor";
constexpr const char* Interface = "org.deskwaifu.Cursor";
constexpr const char* KWin = "org.kde.KWin";
constexpr const char* Scripting = "org.kde.kwin.Scripting";

bool Number(const char* text, double& value)
{
    if (!text || !*text || std::strlen(text) > 64) return false;
    char* end = nullptr;
    errno = 0;
    value = std::strtod(text, &end);
    return errno == 0 && *end == '\0' && std::isfinite(value);
}
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
