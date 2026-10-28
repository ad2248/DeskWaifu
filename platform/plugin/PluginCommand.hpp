#pragma once

#include <QString>

// Execute a plugin-owned command, or return -1 to continue normal startup.
int RunPluginCommand(int argc, char* argv[], const QString& defaultPluginRoot);
