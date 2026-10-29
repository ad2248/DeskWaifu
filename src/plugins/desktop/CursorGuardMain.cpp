#include "processor/input/KWinCursorTracker.hpp"

int main(int argc, char* argv[])
{
    return argc == 4 ? RunKWinCursorGuard(argv[1], argv[2], argv[3]) : 1;
}
