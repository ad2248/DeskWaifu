#include "LAppIPC.hpp"
#include "ExternalControlState.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

struct FakeCommands : ICommandService
{
    int calls = 0;
    int releases = 0;
    std::string source;
    ExternalControlState controls;
    std::string Execute(const std::string& json, const std::string& connection) override
    {
        ++calls;
        source = connection;
        if (json == "mouth") controls.SetMouth(connection, ExternalControlState::Lifetime::Stream, 0.8f);
        if (json == "look") controls.SetLook(connection, ExternalControlState::Lifetime::Stream, 0.2f, 0.3f);
        return json == "first" ? "one" : "two";
    }
    void Disconnected(const std::string& connection) override
    {
        if (connection == source) ++releases;
        controls.Release(connection);
    }
};

int main()
{
    char path[] = "/tmp/deskwaifu-ipc-test-XXXXXX";
    if (!mkdtemp(path)) return 1;
    const char* previous = getenv("XDG_RUNTIME_DIR");
    const std::string saved = previous ? previous : "";
    setenv("XDG_RUNTIME_DIR", path, 1);

    FakeCommands commands;
    LAppIPC transport;
    transport.SetCommandService(commands);
    bool ok = transport.Initialize();
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    timeval limit{2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, transport.GetSocketPath().c_str(), sizeof(addr.sun_path) - 1);
    if (ok) ok = connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) ok = write(client, "fir", 3) == 3;
    transport.Poll();
    if (commands.calls != 0) ok = false;
    if (ok) ok = write(client, "st\nsecond\n", 10) == 10;
    transport.Poll();
    char response[64]{};
    if (ok) ok = read(client, response, sizeof(response)) == 8;
    if (std::string(response) != "one\ntwo\n" || commands.calls != 2 ||
        commands.source.find("stream:") != 0) ok = false;
    const std::string firstSource = commands.source;
    close(client);
    transport.Poll();
    if (commands.releases != 1) ok = false;
    if (commands.controls.Frame().hasMouth) ok = false;
    int streaming = socket(AF_UNIX, SOCK_STREAM, 0);
    setsockopt(streaming, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    if (ok) ok = connect(streaming, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) ok = write(streaming, "mouth\nlook\n", 11) == 11;
    transport.Poll();
    if (ok) ok = read(streaming, response, sizeof(response)) == 8;
    auto frame = commands.controls.Frame();
    if (!frame.hasMouth || frame.mouth != 0.8f || !frame.hasLook || frame.lookX != 0.2f ||
        commands.source.find("stream:") != 0 || commands.source == firstSource) ok = false;
    close(streaming);
    transport.Poll();
    if (commands.releases != 2 || commands.controls.Frame().hasMouth ||
        commands.controls.Frame().hasLook) ok = false;

    int retained = socket(AF_UNIX, SOCK_STREAM, 0);
    setsockopt(retained, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    if (ok) ok = connect(retained, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) ok = write(retained, "mouth\n", 6) == 6;
    transport.Poll();
    if (ok) ok = read(retained, response, sizeof(response)) == 4;
    if (!commands.controls.Frame().hasMouth) ok = false;
    transport.Release();
    if (commands.releases != 3 || commands.controls.Frame().hasMouth ||
        commands.controls.Frame().hasLook) ok = false;
    close(retained);
    if (access(addr.sun_path, F_OK) == 0) ok = false;
    if (previous) setenv("XDG_RUNTIME_DIR", saved.c_str(), 1);
    else unsetenv("XDG_RUNTIME_DIR");
    rmdir(path);
    return ok ? 0 : 1;
}
