#include "SyntheticPaste.h"

#include <QGuiApplication>

// Platform headers define macros (None, Bool, KeyPress, ...) that clash with Qt: include last.
#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(CLIPITTO_HAVE_XTEST)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#endif

namespace clipitto {

#if defined(Q_OS_WIN)

namespace {

HWND g_pasteTarget = nullptr;

} // namespace

void rememberPasteTarget()
{
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    // Our own popup (already open) is no target: keep the previous one.
    if (window && process != GetCurrentProcessId()) {
        g_pasteTarget = window;
    }
}

bool pasteWithSyntheticKeys(PasteKeys keys)
{
    if (g_pasteTarget && IsWindow(g_pasteTarget) && GetForegroundWindow() != g_pasteTarget) {
        // Allowed: our process had the foreground until the popup hid a moment ago.
        SetForegroundWindow(g_pasteTarget);
    }

    const bool shiftInsert = keys == PasteKeys::ShiftInsert;
    const WORD modifier = shiftInsert ? VK_SHIFT : VK_CONTROL;
    const WORD key = shiftInsert ? VK_INSERT : 'V';
    const DWORD keyFlags = shiftInsert ? KEYEVENTF_EXTENDEDKEY : 0; // Insert is an extended key

    INPUT inputs[4] = {};
    for (INPUT& input : inputs) {
        input.type = INPUT_KEYBOARD;
    }
    inputs[0].ki.wVk = modifier;
    inputs[1].ki.wVk = key;
    inputs[1].ki.dwFlags = keyFlags;
    inputs[2].ki.wVk = key;
    inputs[2].ki.dwFlags = keyFlags | KEYEVENTF_KEYUP;
    inputs[3].ki.wVk = modifier;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

#elif defined(CLIPITTO_HAVE_XTEST)

void rememberPasteTarget() {}

bool pasteWithSyntheticKeys(PasteKeys keys)
{
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
}

#else

void rememberPasteTarget() {}

bool pasteWithSyntheticKeys(PasteKeys)
{
    return false;
}

#endif

} // namespace clipitto
