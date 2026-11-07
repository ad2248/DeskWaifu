#include "QtLogging.hpp"

#include <QDir>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QUuid>
#include <cstdio>

namespace
{
QFile logFile;
QMutex logMutex;

void HandleMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QString formatted = qFormatLogMessage(type, context, message);
    QMutexLocker locker(&logMutex);
    if (logFile.isOpen())
    {
        QTextStream stream(&logFile);
        stream << formatted << '\n';
        stream.flush();
    }
    const QByteArray stderrMessage = (formatted + '\n').toLocal8Bit();
    std::fwrite(stderrMessage.constData(), 1, static_cast<size_t>(stderrMessage.size()), stderr);
    std::fflush(stderr);
}
}

QString InitializeQtLogging(const QString& fileName)
{
    const QByteArray configured = qgetenv("DESKWAIFU_LOG_DIR");
    const QString directory = configured.isEmpty()
        ? QDir::temp().filePath(QStringLiteral("log/waifudesk_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces)))
        : QFile::decodeName(configured);

    QDir().mkpath(directory);
    const QString path = QDir(directory).filePath(fileName);
    {
        QMutexLocker locker(&logMutex);
        logFile.close();
        logFile.setFileName(path);
        logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    }
    if (configured.isEmpty()) qputenv("DESKWAIFU_LOG_DIR", QFile::encodeName(directory));
    qInstallMessageHandler(HandleMessage);
    return path;
}
