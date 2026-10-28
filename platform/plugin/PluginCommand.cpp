#include "PluginCommand.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

int RunPluginCommand(int argc, char* argv[], const QString& pluginRoot)
{
    if (pluginRoot.isEmpty()) return -1;
    const QDir root(pluginRoot);
    const auto plugins = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (int index = 1; index < argc; ++index) {
        const auto argument = QString::fromLocal8Bit(argv[index]);
        if (argument.isEmpty() || argument.contains('/') || argument == "." || argument == "..") continue;
        QStringList providers;
        for (const auto& plugin : plugins) {
            const QFileInfo command(root.filePath(plugin + "/commands/" + argument));
            if (command.isFile() && command.isExecutable()) providers.append(command.absoluteFilePath());
        }
        if (providers.isEmpty()) continue;
        if (providers.size() != 1) {
            std::fprintf(stderr, "[Plugins] Ambiguous plugin command: %s\n", argv[index]);
            return 1;
        }
        const auto path = QFile::encodeName(providers.front());
        ::execv(path.constData(), argv);
        std::fprintf(stderr, "[Plugins] Cannot execute command: %s\n", std::strerror(errno));
        return 1;
    }
    return -1;
}
