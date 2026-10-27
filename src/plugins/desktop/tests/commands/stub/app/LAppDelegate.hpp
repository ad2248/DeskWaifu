#pragma once

#include "processor/Processor.hpp"
#include <memory>

// Replace only the desktop runtime boundary, not DesktopCommands or its entry.
class LAppDelegate
{
public:
    static LAppDelegate* GetInstance();
    static void ReleaseInstance();
    bool Initialize(ProcessorEventLoop& loop);
    bool StartFrames(std::function<void(bool)>) { return true; }
    bool Release();
private:
    std::unique_ptr<Processor> _boundary;
};
