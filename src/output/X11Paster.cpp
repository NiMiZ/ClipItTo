#include "X11Paster.h"

#include <QGuiApplication>

// X11 headers define macros (None, Bool, KeyPress, ...) that clash with Qt: include them last.
#ifdef CLIPITTO_HAVE_XTEST
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#endif

namespace clipitto {

bool pasteViaXTest([[maybe_unused]] PasteKeys keys)
{
#ifdef CLIPITTO_HAVE_XTEST
    auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    Display* display = x11 ? x11->display() : nullptr;
    if (!display) {
        return false;
    }
    const KeySym modifier = keys == PasteKeys::ShiftInsert ? XK_Shift_L : XK_Control_L;
    const KeySym key = keys == PasteKeys::ShiftInsert ? XK_Insert : XK_v;
    const KeyCode modifierCode = XKeysymToKeycode(display, modifier);
    const KeyCode keyCode = XKeysymToKeycode(display, key);
    if (!modifierCode || !keyCode) {
        return false;
    }
    XTestFakeKeyEvent(display, modifierCode, True, CurrentTime);
    XTestFakeKeyEvent(display, keyCode, True, CurrentTime);
    XTestFakeKeyEvent(display, keyCode, False, CurrentTime);
    XTestFakeKeyEvent(display, modifierCode, False, CurrentTime);
    XFlush(display);
    return true;
#else
    return false;
#endif
}

} // namespace clipitto
