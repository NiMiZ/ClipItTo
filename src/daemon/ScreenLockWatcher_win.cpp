#include "ScreenLockWatcher.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMetaObject>

#include <windows.h>
#include <wtsapi32.h>

Q_LOGGING_CATEGORY(lcScreenLock, "clipitto.screenlock")

namespace clipitto {

namespace {

constexpr wchar_t kWindowClass[] = L"ClipItToScreenLockWatcher";

// A hidden top-level window (never shown) that Windows tells about the session and the power state. A message-only
// window would not get the power broadcast. Everything it owns is released by the destructor, in reverse order.
class WindowsScreenLockWatcher : public ScreenLockWatcher {
public:
    ~WindowsScreenLockWatcher() override { stop(); }

    bool start(QString* why) override
    {
        if (m_hwnd) {
            return true;
        }
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &WindowsScreenLockWatcher::windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc); // fails harmlessly when the class exists from an earlier start()
        m_hwnd = CreateWindowExW(0, kWindowClass, L"ClipItTo", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!m_hwnd) {
            fail(why, QStringLiteral("could not create the notification window"));
            return false;
        }
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (!WTSRegisterSessionNotification(m_hwnd, NOTIFY_FOR_THIS_SESSION)) {
            qCInfo(lcScreenLock) << "WTSRegisterSessionNotification failed, error" << GetLastError()
                                 << ": session lock is not watched (sleep still is)";
        } else {
            m_registered = true;
        }
        // The window alone still gives the sleep broadcast, so this counts as watching.
        return true;
    }

    void stop() override
    {
        if (m_registered) {
            WTSUnRegisterSessionNotification(m_hwnd);
            m_registered = false;
        }
        if (m_hwnd) {
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    bool isWatching() const override { return m_hwnd != nullptr; }

private:
    static void fail(QString* why, const QString& text)
    {
        if (why) {
            *why = text;
        }
    }

    // Runs on the GUI thread inside the message pump; touches no widgets: it only queues the signal.
    void post(const char* reason)
    {
        QMetaObject::invokeMethod(
            this, [this, text = QString::fromLatin1(reason)] { emit locked(text); }, Qt::QueuedConnection);
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<WindowsScreenLockWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            if (message == WM_WTSSESSION_CHANGE) {
                switch (wParam) {
                case WTS_SESSION_LOCK:
                    self->post("screen lock");
                    break;
                case WTS_CONSOLE_DISCONNECT:
                case WTS_REMOTE_DISCONNECT:
                    self->post("disconnect");
                    break;
                default: // WTS_SESSION_UNLOCK and the rest are ignored on purpose
                    break;
                }
                return 0;
            }
            if (message == WM_POWERBROADCAST && wParam == PBT_APMSUSPEND) {
                self->post("sleep");
                return TRUE;
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND m_hwnd = nullptr;
    bool m_registered = false;
};

} // namespace

std::unique_ptr<ScreenLockWatcher> createScreenLockWatcher()
{
    return std::make_unique<WindowsScreenLockWatcher>();
}

} // namespace clipitto
