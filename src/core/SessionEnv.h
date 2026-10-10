#pragma once

#include <QString>

namespace clipitto {

// What kind of graphical session we are running in; drives capture backend selection.
struct SessionEnv {
    enum class Display { Wayland, X11, Unknown };

    Display display = Display::Unknown;
    bool isGnome = false;

    static SessionEnv detect(); // from the process environment

    // XDG_SESSION_TYPE wins; WAYLAND_DISPLAY / DISPLAY are the fallback.
    // XDG_CURRENT_DESKTOP is colon-separated, e.g. "ubuntu:GNOME".
    static SessionEnv from(const QString& sessionType, const QString& currentDesktop,
                           bool hasWaylandDisplay, bool hasX11Display);
};

} // namespace clipitto
