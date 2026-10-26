#include "SourceApp.h"

#include <QGuiApplication>

// Xlib defines macros (None, Bool, Status, ...) that clash with Qt: include last.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace clipitto::capture {

namespace {

int ignoreXError(Display*, XErrorEvent*)
{
    return 0; // the window went away meanwhile: no name, no crash
}

QString classOf(Display* display, Window window)
{
    QString name;
    XClassHint hint{nullptr, nullptr};
    if (window != 0 && XGetClassHint(display, window, &hint) != 0) {
        name = QString::fromLocal8Bit(hint.res_class ? hint.res_class : hint.res_name);
    }
    if (hint.res_name) {
        XFree(hint.res_name);
    }
    if (hint.res_class) {
        XFree(hint.res_class);
    }
    return name;
}

Window activeWindow(Display* display)
{
    const Atom property = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
    if (property == 0) {
        return 0;
    }
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    Window window = 0;
    if (XGetWindowProperty(display, DefaultRootWindow(display), property, 0, 1, False, XA_WINDOW, &type, &format,
                           &count, &remaining, &data)
            == Success
        && data) {
        if (count == 1 && format == 32) {
            window = *reinterpret_cast<Window*>(data);
        }
        XFree(data);
    }
    return window;
}

} // namespace

QString clipboardSourceApp()
{
    auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    Display* display = x11 ? x11->display() : nullptr;
    if (!display) {
        return {}; // not an X11 session
    }
    XErrorHandler previous = XSetErrorHandler(ignoreXError);
    // The selection owner is often a hidden helper window without a class (GTK): then the
    // window that had focus when the copy was made, which is the one still active.
    const Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    QString name = classOf(display, XGetSelectionOwner(display, clipboard));
    if (name.isEmpty()) {
        name = classOf(display, activeWindow(display));
    }
    XSync(display, False);
    XSetErrorHandler(previous);
    return name;
}

} // namespace clipitto::capture
