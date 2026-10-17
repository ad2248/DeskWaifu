#include "KWinCursorTracker.hpp"

#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Line " << __LINE__ << ": " << #condition << '\n'; return 1; \
} } while (false)

namespace {
template<typename Predicate> bool Wait(Predicate predicate, int milliseconds = 4000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        usleep(5000);
    }
    return false;
}

bool ScriptLoaded(const std::string& name)
{
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) return true;
    sd_bus_message* reply = nullptr;
    int loaded = 1;
    const int result = sd_bus_call_method(bus, "org.kde.KWin", "/Scripting",
        "org.kde.kwin.Scripting", "isScriptLoaded", nullptr, &reply, "s", name.c_str());
    if (result >= 0) sd_bus_message_read(reply, "b", &loaded);
    sd_bus_message_unref(reply);
    sd_bus_flush_close_unref(bus);
    return loaded;
}

// Uses a private session bus via dbus-run-session; never installs scripts or
// touches the user's real KWin. The real compositor smoke test is opt-in.
struct FakeKWin
{
    std::atomic<bool> ready{false}, done{false}, loaded{false}, failLoad{false};
    std::atomic<bool> restart{false}, invalid{false}, silent{false};
    std::atomic<bool> delayLoad{false};
    std::atomic<bool> memoryFileClosed{false};
    std::atomic<int> loads{0}, unloads{0};
    std::thread thread;
    std::string destination, output, file, script;
    bool running = false;
    sd_bus_message* delayedLoad = nullptr;
    std::chrono::steady_clock::time_point replyAt{};

    FakeKWin() { thread = std::thread([this] { Run(); }); }
    ~FakeKWin() { done = true; thread.join(); }

    static int Load(sd_bus_message* message, void* data, sd_bus_error*)
    {
        auto& self = *static_cast<FakeKWin*>(data);
        if (self.failLoad || self.loaded) return sd_bus_reply_method_return(message, "i", -1);
        const char *path, *name;
        if (sd_bus_message_read(message, "ss", &path, &name) < 0) return -1;
        std::ifstream input(path);
        self.script.assign(std::istreambuf_iterator<char>(input), {});
        if (self.script.find("NameHasOwner") == std::string::npos ||
            self.script.find("unloadScript") == std::string::npos ||
            self.script.find(name) == std::string::npos)
            return sd_bus_reply_method_return(message, "i", -1);
        self.file = path;
        self.destination = sd_bus_message_get_sender(message);
        self.loaded = true;
        ++self.loads;
        if (self.delayLoad.exchange(false)) {
            self.delayedLoad = sd_bus_message_ref(message);
            self.replyAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            return 1; // Registered, but the reply arrives after the client's 250 ms timeout.
        }
        return sd_bus_reply_method_return(message, "i", 0);
    }
    static int Unload(sd_bus_message* message, void* data, sd_bus_error*)
    {
        auto& self = *static_cast<FakeKWin*>(data);
        self.loaded = false;
        self.running = false;
        ++self.unloads;
        return sd_bus_reply_method_return(message, "b", 1);
    }
    static int RunScript(sd_bus_message* message, void* data, sd_bus_error*)
    {
        static_cast<FakeKWin*>(data)->running = true;
        return sd_bus_reply_method_return(message, "");
    }
    static int Reply(sd_bus_message* message, void* data, sd_bus_error*)
    {
        const char* name = nullptr;
        if (sd_bus_message_read(message, "s", &name) > 0)
            static_cast<FakeKWin*>(data)->output = name;
        auto& self = *static_cast<FakeKWin*>(data);
        if (access(self.file.c_str(), F_OK) < 0) self.memoryFileClosed = true;
        return 0;
    }
    void Run()
    {
        sd_bus* bus = nullptr;
        if (sd_bus_open_user(&bus) < 0) return;
        static const sd_bus_vtable scripting[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("loadScript", "ss", "i", Load, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("unloadScript", "s", "b", Unload, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        static const sd_bus_vtable scriptObject[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("run", "", "", RunScript, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        sd_bus_add_object_vtable(bus, nullptr, "/Scripting", "org.kde.kwin.Scripting", scripting, this);
        sd_bus_add_object_vtable(bus, nullptr, "/Scripting/Script0", "org.kde.kwin.Script", scriptObject, this);
        if (sd_bus_request_name(bus, "org.kde.KWin", 0) < 0) {
            sd_bus_unref(bus);
            return;
        }
        ready = true;
        auto next = std::chrono::steady_clock::now();
        while (!done) {
            if (restart.exchange(false)) {
                sd_bus_release_name(bus, "org.kde.KWin");
                loaded = false;
                running = false;
                sd_bus_request_name(bus, "org.kde.KWin", 0);
            }
            while (sd_bus_process(bus, nullptr) > 0) {}
            if (delayedLoad && std::chrono::steady_clock::now() >= replyAt) {
                sd_bus_reply_method_return(delayedLoad, "i", 0);
                delayedLoad = sd_bus_message_unref(delayedLoad);
            }
            if (running && !silent && std::chrono::steady_clock::now() >= next) {
                const bool second = output == "B";
                sd_bus_call_method_async(bus, nullptr, destination.c_str(), "/org/deskwaifu/Cursor",
                    "org.deskwaifu.Cursor", "Sample", Reply, this, "sssssss",
                    invalid ? "nan" : "50", "300", output.c_str(),
                    second ? "1000" : "-200", second ? "0" : "100",
                    second ? "800" : "1000", second ? "600" : "800");
                next = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
            }
            usleep(1000);
        }
        sd_bus_message_unref(delayedLoad);
        sd_bus_flush_close_unref(bus);
    }
};
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--script") {
        std::cout << KWinCursorScript(":1.42", "deskwaifu-test");
        return 0;
    }
    KWinCursorTracker tracker;
    double x = 0, y = 0;
    if (argc > 1 && std::string(argv[1]) == "--live") {
        CHECK(argc == 3);
        CHECK(tracker.Start());
        std::cout << "SCRIPT=" << tracker.ScriptName() << std::endl;
        CHECK(Wait([&] {
            tracker.Poll(argv[2]);
            return tracker.LocalPosition(2000, 1000, x, y);
        }));
        std::cout << "KWin sample (render target 2000x1000): " << x << ", " << y << std::endl;
        const auto name = tracker.ScriptName();
        CHECK(ScriptLoaded(name));
        tracker.Stop();
        CHECK(Wait([&] { return !ScriptLoaded(name); }));
        std::cout << "Normal shutdown unloaded the transient script" << std::endl;

        int pipeFds[2];
        CHECK(pipe(pipeFds) == 0);
        const pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            close(pipeFds[0]);
            KWinCursorTracker crashed;
            if (!crashed.Start() || !Wait([&] {
                crashed.Poll(argv[2]);
                return crashed.LocalPosition(2000, 1000, x, y);
            })) _exit(1);
            const auto childName = crashed.ScriptName();
            if (write(pipeFds[1], childName.data(), childName.size()) < 0) _exit(1);
            close(pipeFds[1]);
            for (;;) { crashed.Poll(argv[2]); usleep(5000); }
        }
        close(pipeFds[1]);
        char buffer[256];
        const auto size = read(pipeFds[0], buffer, sizeof(buffer));
        close(pipeFds[0]);
        const std::string childName = size > 0 ? std::string(buffer, size) : "";
        // Always reap the child, including when startup failed.
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        CHECK(!childName.empty());
        CHECK(Wait([&] { return !ScriptLoaded(childName); }, 6000));
        std::cout << "SIGKILL watchdog unloaded the orphaned script" << std::endl;
        return 0;
    }
    CHECK(!tracker.Start()); // No KWin yet: a late compositor can recover.
    CHECK(!tracker.LocalPosition(2000, 1600, x, y));
    FakeKWin kwin;
    CHECK(Wait([&] { return kwin.ready.load(); }));
    CHECK(Wait([&] {
        tracker.Poll("A");
        return tracker.LocalPosition(2000, 1600, x, y);
    }));
    CHECK(x == 500 && y == 400); // Negative output origin and 2x target scale.
    CHECK(Wait([&] { return kwin.memoryFileClosed.load(); }));
    CHECK(tracker.Start()); // Starting an already running tracker is idempotent.
    CHECK(kwin.loads == 1);
    sd_bus* intruder = nullptr;
    CHECK(sd_bus_open_user(&intruder) >= 0);
    const auto prefix = std::string("deskwaifu-cursor-").size();
    const auto destination = tracker.ScriptName().substr(prefix, tracker.ScriptName().rfind('-') - prefix);
    int rejected = 0;
    CHECK(sd_bus_call_method_async(intruder, nullptr, destination.c_str(), "/org/deskwaifu/Cursor",
        "org.deskwaifu.Cursor", "Sample",
        [](sd_bus_message* reply, void* data, sd_bus_error*) {
            *static_cast<int*>(data) = sd_bus_message_is_method_error(reply, SD_BUS_ERROR_ACCESS_DENIED) ? 1 : -1;
            return 0;
        }, &rejected, "sssssss", "0", "0", "A", "0", "0", "100", "100") >= 0);
    CHECK(Wait([&] {
        tracker.Poll("A");
        while (sd_bus_process(intruder, nullptr) > 0) {}
        return rejected != 0;
    }));
    CHECK(rejected == 1); // Other session-bus clients cannot inject gaze samples.
    sd_bus_flush_close_unref(intruder);
    CHECK(!tracker.LocalPosition(0, 1600, x, y));
    tracker.Poll("B");
    CHECK(!tracker.LocalPosition(2000, 1600, x, y)); // Never reuse another screen's geometry.
    CHECK(Wait([&] {
        tracker.Poll("B");
        return tracker.LocalPosition(1000, 750, x, y);
    }));
    CHECK(x == -1187.5 && y == 375); // Cursor on another screen, 125% target scale.
    kwin.invalid = true;
    usleep(1100000);
    tracker.Poll("B");
    CHECK(!tracker.LocalPosition(1000, 750, x, y)); // Invalid/stale samples are not usable.
    kwin.invalid = false;
    kwin.restart = true;
    CHECK(Wait([&] {
        tracker.Poll("A");
        return kwin.loads >= 2 && tracker.LocalPosition(2000, 1600, x, y);
    }));
    kwin.silent = true;
    CHECK(Wait([&] { tracker.Poll("A"); return kwin.unloads == 1; }, 5000));
    CHECK(!tracker.LocalPosition(2000, 1600, x, y));
    kwin.silent = false;
    CHECK(Wait([&] {
        tracker.Poll("A");
        return tracker.LocalPosition(2000, 1600, x, y);
    }));
    tracker.Stop();
    CHECK(!tracker.LocalPosition(2000, 1600, x, y));
    CHECK(kwin.unloads == 2 && !kwin.loaded);

    kwin.failLoad = true;
    CHECK(!tracker.Start());
    CHECK(!tracker.Error().empty());
    kwin.failLoad = false;
    CHECK(Wait([&] {
        tracker.Poll("A");
        return tracker.LocalPosition(2000, 1600, x, y);
    }));
    tracker.Stop();
    CHECK(kwin.unloads == 3 && !kwin.loaded);

    // A lost loadScript reply must not leave a registered, unstarted script
    // blocking all subsequent loads with the same name.
    kwin.delayLoad = true;
    CHECK(!tracker.Start());
    CHECK(kwin.loaded);
    CHECK(Wait([&] {
        tracker.Poll("A");
        return tracker.LocalPosition(2000, 1600, x, y);
    }, 7000));
    tracker.Stop();
    CHECK(kwin.unloads == 5 && !kwin.loaded);

    // Shutdown immediately after an ambiguous load must clean up even though
    // run() was never called and no script-side watchdog could have started.
    kwin.delayLoad = true;
    CHECK(!tracker.Start());
    CHECK(kwin.loaded);
    tracker.Stop();
    CHECK(kwin.unloads == 6 && !kwin.loaded);
    return 0;
}
