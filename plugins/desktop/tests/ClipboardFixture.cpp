#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <signal.h>
#include <string>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc != 3 || std::string(argv[1]) != "--type" || std::string(argv[2]) != "text/plain;charset=utf-8") {
        return 90;
    }
    if (const char* path = std::getenv("DESKWAIFU_CLIPBOARD_FIXTURE_CLOSE_STDIN")) {
        {
            std::ofstream out(path);
            out << getpid();
        }
        close(STDIN_FILENO);
        sleep(30);
        return 0;
    }
    const std::string text { std::istreambuf_iterator<char>(std::cin), { } };
    if (text.rfind("tree:", 0) == 0) {
        const auto path = text.substr(5);
        const auto child = fork();
        if (child < 0) {
            return 91;
        }
        if (child == 0) {
            {
                std::ofstream out(path + ".child");
                out << getpid();
            }
            sleep(30);
            _exit(0);
        }
        {
            std::ofstream out(path);
            out << getpid();
        }
        sleep(30);
    }
    if (text.rfind("hang:", 0) == 0) {
        {
            std::ofstream out(text.substr(5));
            out << getpid();
        }
        sleep(30);
    }
    if (text.rfind("capture:", 0) == 0) {
        const auto end = text.find('\n');
        std::ofstream out(text.substr(8, end - 8), std::ios::binary);
        out << text.substr(end + 1);
    }
    if (text == "fail") {
        return 3;
    }
    if (text == "crash") {
        raise(SIGKILL);
    }
    return 0;
}
