#include "SyntheticPaste.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QtGlobal>

// Platform headers define macros (None, Bool, KeyPress, ...) that clash with Qt: include last.
#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include "MacPaste.h"
#elif defined(CLIPITTO_HAVE_XTEST)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#endif

namespace clipitto {

#if defined(Q_OS_WIN)

namespace {

HWND g_pasteTarget = nullptr;
PasteTarget g_target = PasteTarget::unsupported(); // what to call it: executable name and window class, no title

QStringList namesOf(HWND window, DWORD process)
{
    QStringList ids;
    if (HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process)) {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = DWORD(sizeof(path) / sizeof(path[0]));
        if (QueryFullProcessImageNameW(handle, 0, path, &size)) {
            ids << QFileInfo(QString::fromWCharArray(path, int(size))).completeBaseName();
        }
        CloseHandle(handle);
    }
    wchar_t cls[256] = {};
    const int length = GetClassNameW(window, cls, int(sizeof(cls) / sizeof(cls[0])));
    if (length > 0) {
        ids << QString::fromWCharArray(cls, length);
    }
    return ids;
}

} // namespace

void rememberPasteTarget()
{
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    // Our own popup (already open) is no target: keep the previous one.
    if (window && process != GetCurrentProcessId()) {
        g_pasteTarget = window;
        const QStringList ids = namesOf(window, process);
        g_target = ids.isEmpty() ? PasteTarget::unknown() : PasteTarget::known(ids);
    } else if (g_target.state == PasteTarget::State::Unsupported) {
        g_target = PasteTarget::unknown();
    }
}

PasteTarget rememberedPasteTarget()
{
    return g_target;
}

bool pasteWithSyntheticKeys(PasteKeys keys)
{
    if (g_pasteTarget && IsWindow(g_pasteTarget) && GetForegroundWindow() != g_pasteTarget) {
        // Allowed: our process had the foreground until the popup hid a moment ago.
        SetForegroundWindow(g_pasteTarget);
    }

    const bool shiftInsert = keys == PasteKeys::ShiftInsert;
    const WORD key = shiftInsert ? VK_INSERT : 'V';
    const DWORD keyFlags = shiftInsert ? KEYEVENTF_EXTENDEDKEY : 0; // Insert is an extended key
    WORD modifiers[2] = {shiftInsert ? VK_SHIFT : VK_CONTROL, 0};
    if (keys == PasteKeys::CtrlShiftV) {
        modifiers[1] = VK_SHIFT;
    }
    const UINT count = modifiers[1] ? 2 : 1;

    // Modifiers down, key down, key up, modifiers up (reverse order).
    INPUT inputs[6] = {};
    UINT n = 0;
    const auto add = [&](WORD vk, DWORD flags) {
        inputs[n].type = INPUT_KEYBOARD;
        inputs[n].ki.wVk = vk;
        inputs[n].ki.dwFlags = flags;
        ++n;
    };
    for (UINT i = 0; i < count; ++i) {
        add(modifiers[i], 0);
    }
    add(key, keyFlags);
    add(key, keyFlags | KEYEVENTF_KEYUP);
    for (UINT i = count; i-- > 0;) {
        add(modifiers[i], KEYEVENTF_KEYUP);
    }
    return SendInput(n, inputs, sizeof(INPUT)) == n;
}

#elif defined(Q_OS_MACOS)

void rememberPasteTarget()
{
    macRememberPasteTarget();
}

PasteTarget rememberedPasteTarget()
{
    // The app is known (NSWorkspace) but ⌘V is the paste key of every Mac app, terminals included:
    // there is nothing to choose.
    return PasteTarget::unsupported();
}

bool pasteWithSyntheticKeys(PasteKeys)
{
    return macPasteCommandV(); // always ⌘V: Ctrl+V / Shift+Insert are not Mac paste keys
}

#elif defined(CLIPITTO_HAVE_XTEST)

namespace {

PasteTarget g_target = PasteTarget::unsupported();

int ignoreXError(Display*, XErrorEvent*)
{
    return 0; // the window went away meanwhile: no name, no crash
}

// The WM_CLASS of the window that has the focus: instance and class names ("xterm", "XTerm").
QStringList activeWindowClass(Display* display)
{
    QStringList ids;
    const Atom property = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
    if (property == 0) {
        return ids; // no EWMH window manager
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
    XClassHint hint{nullptr, nullptr};
    if (window != 0 && XGetClassHint(display, window, &hint) != 0) {
        if (hint.res_name) {
            ids << QString::fromLocal8Bit(hint.res_name);
        }
        if (hint.res_class) {
            ids << QString::fromLocal8Bit(hint.res_class);
        }
    }
    if (hint.res_name) {
        XFree(hint.res_name);
    }
    if (hint.res_class) {
        XFree(hint.res_class);
    }
    return ids;
}

} // namespace

void rememberPasteTarget()
{
    auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    Display* display = x11 ? x11->display() : nullptr;
    // On a Wayland session the X server only knows the XWayland windows: its "active window" would be wrong
    // for every other app, so say nothing rather than guess.
    if (!display || qgetenv("XDG_SESSION_TYPE") == "wayland") {
        g_target = PasteTarget::unsupported();
        return;
    }
    XErrorHandler previous = XSetErrorHandler(ignoreXError);
    const QStringList ids = activeWindowClass(display);
    XSync(display, False);
    XSetErrorHandler(previous);
    // Our own popup (already open) is no target: keep the previous one.
    if (ids.contains(QLatin1String("clipitto"), Qt::CaseInsensitive)
        || ids.contains(QLatin1String("clipitto-daemon"), Qt::CaseInsensitive)) {
        if (g_target.state == PasteTarget::State::Unsupported) {
            g_target = PasteTarget::unknown();
        }
        return;
    }
    g_target = ids.isEmpty() ? PasteTarget::unknown() : PasteTarget::known(ids);
}

PasteTarget rememberedPasteTarget()
{
    return g_target;
}

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
    const KeyCode extraCode = keys == PasteKeys::CtrlShiftV ? XKeysymToKeycode(display, XK_Shift_L) : KeyCode(0);
    const KeyCode keyCode = XKeysymToKeycode(display, key);
    if (!modifierCode || !keyCode || (keys == PasteKeys::CtrlShiftV && !extraCode)) {
        return false;
    }
    XTestFakeKeyEvent(display, modifierCode, True, CurrentTime);
    if (extraCode) {
        XTestFakeKeyEvent(display, extraCode, True, CurrentTime);
    }
    XTestFakeKeyEvent(display, keyCode, True, CurrentTime);
    XTestFakeKeyEvent(display, keyCode, False, CurrentTime);
    if (extraCode) {
        XTestFakeKeyEvent(display, extraCode, False, CurrentTime);
    }
    XTestFakeKeyEvent(display, modifierCode, False, CurrentTime);
    XFlush(display);
    return true;
}

#else

void rememberPasteTarget() {}

PasteTarget rememberedPasteTarget()
{
    return PasteTarget::unsupported();
}

bool pasteWithSyntheticKeys(PasteKeys)
{
    return false;
}

#endif

} // namespace clipitto
