#include "SessionEnv.h"

#include <QStringList>
#include <QtGlobal>

namespace clipitto {

SessionEnv SessionEnv::detect()
{
    return from(qEnvironmentVariable("XDG_SESSION_TYPE"), qEnvironmentVariable("XDG_CURRENT_DESKTOP"),
                !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"), !qEnvironmentVariableIsEmpty("DISPLAY"));
}

SessionEnv SessionEnv::from(const QString& sessionType, const QString& currentDesktop,
                            bool hasWaylandDisplay, bool hasX11Display)
{
    SessionEnv env;
    const QString type = sessionType.trimmed().toLower();
    if (type == QLatin1String("wayland")) {
        env.display = Display::Wayland;
    } else if (type == QLatin1String("x11")) {
        env.display = Display::X11;
    } else if (hasWaylandDisplay) {
        env.display = Display::Wayland;
    } else if (hasX11Display) {
        env.display = Display::X11;
    }

    const QStringList desktops = currentDesktop.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    env.isGnome = desktops.contains(QLatin1String("GNOME"), Qt::CaseInsensitive);
    return env;
}

} // namespace clipitto
