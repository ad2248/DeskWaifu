#pragma once

#include <QString>

// Installs the process-wide Qt message handler and truncates the process log
// for this launch. The directory is shared with child processes through
// DESKWAIFU_LOG_DIR when the GUI starts the daemon.
QString InitializeQtLogging(const QString& fileName);
