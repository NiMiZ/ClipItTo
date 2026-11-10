#include "DebugLog.h"

#include "Paths.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <memory>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace clipitto::debuglog {

namespace {

QMutex g_mutex;
QFile* g_file = nullptr;
QString g_path;
qint64 g_max = 0;
qint64 g_written = 0;
QtMessageHandler g_previous = nullptr;

// Opens `path` empty. Unix: created with mode 0600 in one step and never through a symbolic link (O_NOFOLLOW), so the file is
// never readable by others, not even for a moment, and a link can't make us overwrite something else.
std::unique_ptr<QFile> openPrivate(const QString& path)
{
    auto file = std::make_unique<QFile>(path);
#if defined(_WIN32)
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return nullptr;
    }
#else
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || !file->open(fd, QIODevice::WriteOnly, QFile::AutoCloseHandle)) {
        if (fd >= 0) {
            ::close(fd);
        }
        return nullptr;
    }
#endif
    paths::restrictToOwner(path);
    return file;
}

const char* typeName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "debug";
    case QtInfoMsg:
        return "info";
    case QtWarningMsg:
        return "warning";
    case QtCriticalMsg:
        return "critical";
    case QtFatalMsg:
        return "fatal";
    }
    return "?";
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    // Formatted before the lock is taken: the lock is held only for the write.
    const QByteArray line = (QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) + QLatin1Char(' ')
                             + QLatin1String(typeName(type)) + QLatin1Char(' ')
                             + QLatin1String(context.category ? context.category : "default") + QLatin1String(": ") + message)
                                .toUtf8()
        + '\n';
    QtMessageHandler previous = nullptr;
    {
        QMutexLocker lock(&g_mutex);
        previous = g_previous;
        if (g_file) {
            if (g_written + line.size() > g_max) { // the newest lines matter most: keep one older file, start a new one
                g_file->close();
                QFile::remove(g_path + QStringLiteral(".1"));
                QFile::rename(g_path, g_path + QStringLiteral(".1"));
                delete g_file;
                g_file = openPrivate(g_path).release(); // null when that fails: stop writing, stderr still works
                g_written = 0;
            }
            if (g_file) {
                g_file->write(line);
                g_file->flush();
                g_written += line.size();
            }
        }
    }
    if (previous) {
        previous(type, context, message); // stderr as before
    }
}

} // namespace

bool start(const QString& path, qint64 maxBytes)
{
    stop();
    std::unique_ptr<QFile> file = openPrivate(path);
    if (!file) {
        return false;
    }
    QMutexLocker lock(&g_mutex);
    g_file = file.release();
    g_path = path;
    g_max = maxBytes;
    g_written = 0;
    g_previous = qInstallMessageHandler(handler);
    return true;
}

void stop()
{
    QMutexLocker lock(&g_mutex);
    if (!g_file) {
        return;
    }
    // Put back what was there before, unless something else installed its own handler after ours: that one stays.
    const QtMessageHandler current = qInstallMessageHandler(g_previous);
    if (current != handler) {
        qInstallMessageHandler(current);
    }
    g_previous = nullptr;
    g_file->close();
    delete g_file;
    g_file = nullptr;
}

void remove(const QString& path)
{
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".1"));
}

} // namespace clipitto::debuglog
