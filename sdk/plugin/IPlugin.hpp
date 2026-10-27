#pragma once

#include <QString>

class IPlugin
{
public:
    virtual ~IPlugin() = default;
    virtual bool Start(QString* error) { Q_UNUSED(error); return true; }
    // Must handle partial initialization. Stop and join workers before returning.
    virtual void Stop() noexcept = 0;
};
