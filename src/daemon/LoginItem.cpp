#include "LoginItem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace clipitto {

LoginItem::LoginItem(QString plistPath, QString executable)
    : m_path(std::move(plistPath))
    , m_executable(std::move(executable))
{
}

QString LoginItem::defaultPlistPath()
{
    return QDir::home().filePath(QStringLiteral("Library/LaunchAgents/%1.plist").arg(QLatin1String(kLabel)));
}

QString LoginItem::plist() const
{
    // LimitLoadToSessionType: only in the graphical login, not over ssh. KeepAlive is off: a
    // ClipItTo that was quit stays quit until the next login.
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\">\n"
               "<dict>\n"
               "    <key>Label</key>\n"
               "    <string>%1</string>\n"
               "    <key>ProgramArguments</key>\n"
               "    <array>\n"
               "        <string>%2</string>\n"
               "    </array>\n"
               "    <key>RunAtLoad</key>\n"
               "    <true/>\n"
               "    <key>ProcessType</key>\n"
               "    <string>Interactive</string>\n"
               "    <key>LimitLoadToSessionType</key>\n"
               "    <string>Aqua</string>\n"
               "</dict>\n"
               "</plist>\n")
        .arg(QLatin1String(kLabel), m_executable.toHtmlEscaped());
}

bool LoginItem::isEnabled() const
{
    return QFileInfo::exists(m_path);
}

bool LoginItem::setEnabled(bool on, QString* error)
{
    if (!on) {
        if (QFile::exists(m_path) && !QFile::remove(m_path)) {
            if (error) {
                *error = QStringLiteral("could not remove %1").arg(m_path);
            }
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("could not create %1").arg(QFileInfo(m_path).absolutePath());
        }
        return false;
    }
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(plist().toUtf8()) < 0 || !file.commit()) {
        if (error) {
            *error = QStringLiteral("could not write %1: %2").arg(m_path, file.errorString());
        }
        return false;
    }
    return true;
}

} // namespace clipitto
