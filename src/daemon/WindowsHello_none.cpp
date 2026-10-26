#include "WindowsHello.h"

#include <QCoreApplication>

namespace clipitto::hello {

namespace {

QString unsupported()
{
    return QCoreApplication::translate("clipitto::Daemon", "Windows Hello is only on Windows.");
}

} // namespace

bool available(QString* why)
{
    if (why) {
        *why = unsupported();
    }
    return false;
}

QByteArray unlockKey(bool, QString* error)
{
    if (error) {
        *error = unsupported();
    }
    return {};
}

void removeKey() {}

void cancelPending() {}

} // namespace clipitto::hello
