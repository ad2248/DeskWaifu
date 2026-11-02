#include "processor/input/KWinCursorTracker.hpp"

#include <atomic>
#include <clocale>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iostream>
#include <locale>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

extern char** environ;

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

std::string KWinOwner()
{
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) return {};
    sd_bus_message* reply = nullptr;
    const char* owner = nullptr;
    std::string result;
    if (sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", nullptr, &reply, "s", "org.kde.KWin") >= 0 &&
        sd_bus_message_read(reply, "s", &owner) > 0) result = owner;
    sd_bus_message_unref(reply);
    sd_bus_flush_close_unref(bus);
    return result;
}

// 通过 dbus-run-session 使用私有会话总线，不安装脚本，
// 也不操作用户真实的 KWin。真实合成器冒烟测试需显式启用。
struct FakeKWin
{
    std::atomic<bool> ready{false}, done{false}, loaded{false}, failLoad{false};
    std::atomic<bool> restart{false}, invalid{false}, silent{false};
    std::atomic<bool> retainOldConnection{false}, injectOldSample{false};
    std::atomic<bool> oldConnectionClosed{false};
    std::atomic<int> oldSampleResult{0};
    std::atomic<bool> delayLoad{false};
    std::atomic<bool> fractional{false};
    std::atomic<bool> blockRun{false}, runBlocked{false};
    std::atomic<bool> memoryFileClosed{false};
    std::atomic<int> loads{0}, unloads{0}, runs{0}, samples{0};
    std::thread thread;
    std::string destination, output, file, script, registeredName;
    bool running = false;
    std::vector<std::pair<sd_bus_message*, std::chrono::steady_clock::time_point>> delayedLoads;

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
        self.registeredName = name;
        self.destination = sd_bus_message_get_sender(message);
        self.loaded = true;
        ++self.loads;
        if (self.delayLoad.exchange(false)) {
            self.delayedLoads.emplace_back(sd_bus_message_ref(message),
                std::chrono::steady_clock::now() + std::chrono::milliseconds(400));
            return 1; // 注册成功，但回复晚于客户端的 250 毫秒超时。
        }
        return sd_bus_reply_method_return(message, "i", 0);
    }
    static int Unload(sd_bus_message* message, void* data, sd_bus_error*)
    {
        auto& self = *static_cast<FakeKWin*>(data);
        const char* name = nullptr;
        if (sd_bus_message_read(message, "s", &name) < 0) return -1;
        if (!self.loaded || self.registeredName != name)
            return sd_bus_reply_method_return(message, "b", 0);
        self.loaded = false;
        self.running = false;
        ++self.unloads;
        return sd_bus_reply_method_return(message, "b", 1);
    }
    static int RunScript(sd_bus_message* message, void* data, sd_bus_error*)
    {
        auto& self = *static_cast<FakeKWin*>(data);
        if (self.blockRun.exchange(false)) {
            self.runBlocked = true;
            return 1; // 客户端被终止前，不启动脚本或发送回复。
        }
        self.running = true;
        ++self.runs;
        return sd_bus_reply_method_return(message, "");
    }
    static int Reply(sd_bus_message* message, void* data, sd_bus_error*)
    {
        const char* name = nullptr;
        if (sd_bus_message_read(message, "s", &name) > 0) {
            static_cast<FakeKWin*>(data)->output = name;
            ++static_cast<FakeKWin*>(data)->samples;
        }
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
        auto registerService = [&] {
            return sd_bus_add_object_vtable(bus, nullptr, "/Scripting", "org.kde.kwin.Scripting", scripting, this) >= 0 &&
                sd_bus_add_object_vtable(bus, nullptr, "/Scripting/Script0", "org.kde.kwin.Script", scriptObject, this) >= 0 &&
                sd_bus_request_name(bus, "org.kde.KWin", 0) >= 0;
        };
        if (!registerService()) {
            sd_bus_unref(bus);
            return;
        }
        ready = true;
        sd_bus* oldBus = nullptr;
        auto next = std::chrono::steady_clock::now();
        while (!done) {
            if (restart.exchange(false)) {
                // 通常像合成器退出一样断开连接。
                // 仅为独立的旧来源探测保留已释放的连接。
                if (retainOldConnection.exchange(false)) {
                    sd_bus_release_name(bus, "org.kde.KWin");
                    oldBus = bus;
                } else {
                    sd_bus_flush_close_unref(bus);
                }
                bus = nullptr;
                for (const auto& pending : delayedLoads) sd_bus_message_unref(pending.first);
                delayedLoads.clear();
                loaded = false;
                running = false;
                output.clear();
                samples = 0;
                if (sd_bus_open_user(&bus) < 0 || !registerService()) break;
            }
            if (injectOldSample.exchange(false) && oldBus) {
                sd_bus_call_method_async(oldBus, nullptr, destination.c_str(), "/org/deskwaifu/Cursor",
                    "org.deskwaifu.Cursor", "Sample",
                    [](sd_bus_message* reply, void* data, sd_bus_error*) {
                        static_cast<FakeKWin*>(data)->oldSampleResult =
                            sd_bus_message_is_method_error(reply, SD_BUS_ERROR_ACCESS_DENIED) ? 1 : -1;
                        return 0;
                    }, this, "sssssss", "0", "0", "A", "0", "0", "100", "100");
            }
            if (oldBus) {
                while (sd_bus_process(oldBus, nullptr) > 0) {}
                if (oldSampleResult != 0) {
                    oldBus = sd_bus_flush_close_unref(oldBus);
                    oldConnectionClosed = true;
                }
            }
            while (sd_bus_process(bus, nullptr) > 0) {}
            for (auto it = delayedLoads.begin(); it != delayedLoads.end();) {
                if (std::chrono::steady_clock::now() < it->second) { ++it; continue; }
                sd_bus_reply_method_return(it->first, "i", 0);
                sd_bus_message_unref(it->first);
                it = delayedLoads.erase(it);
            }
            if (running && !silent && std::chrono::steady_clock::now() >= next) {
                const bool second = output == "B";
                sd_bus_call_method_async(bus, nullptr, destination.c_str(), "/org/deskwaifu/Cursor",
                    "org.deskwaifu.Cursor", "Sample", Reply, this, "sssssss",
                    invalid ? "nan" : (fractional ? "50.5" : "50"),
                    fractional ? "300.25" : "300", output.c_str(),
                    second ? "1000" : (fractional ? "-200.25" : "-200"),
                    second ? "0" : (fractional ? "100.125" : "100"),
                    second ? "800" : (fractional ? "1000.5" : "1000"),
                    second ? "600" : (fractional ? "800.25" : "800"));
                next = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
            }
            usleep(1000);
        }
        for (const auto& pending : delayedLoads) sd_bus_message_unref(pending.first);
        sd_bus_flush_close_unref(oldBus);
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
    const bool localeTest = argc == 3 && std::string(argv[1]) == "--numeric-locale";
    if (localeTest) {
        std::locale::global(std::locale(argv[2]));
        CHECK(std::setlocale(LC_NUMERIC, argv[2]));
        CHECK(std::string(std::localeconv()->decimal_point) == ",");
    }
    KWinCursorTracker tracker;
    if (argc == 2 && std::string(argv[1]) == "--load-timeout-client") {
        tracker.Start();
        for (;;) pause();
    }
    if (argc == 2 && std::string(argv[1]) == "--restart-client") {
        CHECK(tracker.Start());
        for (;;) { tracker.Poll("A"); usleep(5000); }
    }
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
        // 始终回收子进程，启动失败时也不例外。
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        CHECK(!childName.empty());
        CHECK(Wait([&] { return !ScriptLoaded(childName); }, 6000));
        std::cout << "SIGKILL cleanup unloaded the orphaned script" << std::endl;
        return 0;
    }
    CHECK(!tracker.Start()); // KWin 尚未启动；合成器稍后出现时应能恢复。
    CHECK(!tracker.LocalPosition(2000, 1600, x, y));
    FakeKWin kwin;
    CHECK(Wait([&] { return kwin.ready.load(); }));
    CHECK(Wait([&] {
        tracker.Poll("A");
        return tracker.LocalPosition(2000, 1600, x, y);
    }));
    CHECK(x == 500 && y == 400); // 输出原点为负，目标缩放为两倍。
    if (localeTest) {
        const std::string numericLocale = std::setlocale(LC_NUMERIC, nullptr);
        kwin.fractional = true;
        CHECK(Wait([&] {
            tracker.Poll("A");
            return tracker.LocalPosition(2000, 1600, x, y) &&
                std::abs(x - (50.5 + 200.25) * 2000 / 1000.5) < 1e-9 &&
                std::abs(y - (300.25 - 100.125) * 1600 / 800.25) < 1e-9;
        }));
        CHECK(numericLocale == std::setlocale(LC_NUMERIC, nullptr));
        CHECK(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point() == ',');
        tracker.Stop();
        CHECK(!kwin.loaded);
        return 0;
    }
    CHECK(Wait([&] { return kwin.memoryFileClosed.load(); }));
    CHECK(tracker.Start()); // 重复启动已运行的跟踪器具有幂等性。
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
    CHECK(rejected == 1); // 其他会话总线客户端不能注入视线采样。
    sd_bus_flush_close_unref(intruder);
    CHECK(!tracker.LocalPosition(0, 1600, x, y));
    tracker.Poll("B");
    CHECK(!tracker.LocalPosition(2000, 1600, x, y)); // 不能复用其他屏幕的几何信息。
    CHECK(Wait([&] {
        tracker.Poll("B");
        return tracker.LocalPosition(1000, 750, x, y);
    }));
    CHECK(x == -1187.5 && y == 375); // 光标位于其他屏幕，目标缩放为 125%。
    kwin.invalid = true;
    usleep(1100000);
    tracker.Poll("B");
    CHECK(!tracker.LocalPosition(1000, 750, x, y)); // 无效或过期采样不可用。
    kwin.invalid = false;
    const auto originalOwner = KWinOwner();
    CHECK(!originalOwner.empty());
    kwin.restart = true;
    CHECK(Wait([&] {
        tracker.Poll("A");
        return kwin.loads >= 2 && tracker.LocalPosition(2000, 1600, x, y);
    }));
    CHECK(KWinOwner() != originalOwner && !KWinOwner().empty());
    CHECK(x == 500 && y == 400);
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

    // loadScript 回复丢失后，不能残留已注册但未启动的脚本，
    // 以免阻塞后续所有同名加载。
    kwin.delayLoad = true;
    CHECK(!tracker.Start());
    CHECK(kwin.loaded);
    CHECK(Wait([&] {
        tracker.Poll("A");
        return tracker.LocalPosition(2000, 1600, x, y);
    }, 7000));
    tracker.Stop();
    CHECK(kwin.unloads == 5 && !kwin.loaded);

    // 加载结果不确定后立即退出，也必须完成清理，
    // 即使尚未调用 run()，脚本侧看门狗也未启动。
    kwin.delayLoad = true;
    CHECK(!tracker.Start());
    CHECK(kwin.loaded);
    tracker.Stop();
    CHECK(kwin.unloads == 6 && !kwin.loaded);

    // 在 loadScript 已注册但延迟回复尚未到达时，终止真实客户端。
    // 此时 Stop() 和脚本看门狗都无法协助清理。
    kwin.delayLoad = true;
    const int loadsBeforeCrash = kwin.loads;
    const int runsBeforeCrash = kwin.runs;
    char* childArgs[] = {argv[0], const_cast<char*>("--load-timeout-client"), nullptr};
    pid_t child = -1;
    CHECK(posix_spawn(&child, argv[0], nullptr, nullptr, childArgs, environ) == 0);
    const bool registered = Wait([&] { return kwin.loaded && kwin.loads > loadsBeforeCrash; });
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(registered);
    CHECK(Wait([&] { return !kwin.loaded; }));
    CHECK(kwin.unloads == 7);
    CHECK(kwin.runs == runsBeforeCrash);

    // 也覆盖加载及时回复、随后 run 请求尚未处理的情况。
    kwin.blockRun = true;
    CHECK(posix_spawn(&child, argv[0], nullptr, nullptr, childArgs, environ) == 0);
    const bool blocked = Wait([&] { return kwin.runBlocked.load(); });
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    CHECK(blocked);
    CHECK(Wait([&] { return !kwin.loaded; }));
    CHECK(kwin.unloads == 8 && kwin.runs == runsBeforeCrash);

    // 将轮询客户端重启到另一个唯一所有者，短暂保留旧连接，
    // 验证旧连接已失去发送采样的权限。
    char* restartArgs[] = {argv[0], const_cast<char*>("--restart-client"), nullptr};
    const int loadsBeforeRestart = kwin.loads;
    const int runsBeforeRestart = kwin.runs;
    const int samplesBeforeRestart = kwin.samples;
    CHECK(posix_spawn(&child, argv[0], nullptr, nullptr, restartArgs, environ) == 0);
    const bool started = Wait([&] {
        return kwin.loads > loadsBeforeRestart && kwin.runs > runsBeforeRestart &&
            kwin.samples > samplesBeforeRestart;
    });
    const auto previousOwner = KWinOwner();
    kwin.retainOldConnection = true;
    kwin.restart = true;
    const bool reloaded = Wait([&] {
        return kwin.loads >= loadsBeforeRestart + 2 && kwin.runs >= runsBeforeRestart + 2 &&
            kwin.samples >= 2;
    });
    const auto replacementOwner = KWinOwner();
    kwin.injectOldSample = true;
    const bool replied = Wait([&] { return kwin.oldConnectionClosed.load(); });
    // 即使重启断言失败，也必须回收子进程。SIGKILL 不执行 Stop()，
    // 因此只有面向替代所有者的守护进程才能卸载脚本。
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    CHECK(started && reloaded);
    CHECK(!replacementOwner.empty() && replacementOwner != previousOwner);
    CHECK(replied && kwin.oldSampleResult == 1);
    CHECK(Wait([&] { return !kwin.loaded; }));
    CHECK(kwin.unloads == 9);
    return 0;
}
