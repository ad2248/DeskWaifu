#include "LAppIPC.hpp"

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
    std::string Execute(const std::string& json) override
    {
        ++calls;
        return json == "first" ? "one" : "two";
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
    if (std::string(response) != "one\ntwo\n" || commands.calls != 2) ok = false;
    close(client);
    transport.Poll();
    transport.Release();
    if (access(addr.sun_path, F_OK) == 0) ok = false;
    if (previous) setenv("XDG_RUNTIME_DIR", saved.c_str(), 1);
    else unsetenv("XDG_RUNTIME_DIR");
    rmdir(path);
    return ok ? 0 : 1;
}
