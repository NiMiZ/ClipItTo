#pragma once

#include "CaptureBackend.h"
#include "NativeClipboard.h"

#include <QClipboard>
#include <QTimer>

#include <functional>

namespace clipitto::capture {

// QClipboard change notifications. Complete on X11 (Qt uses XFixes selection events) and
// Windows (clipboard format listener); on Wayland, Qt only sees changes while one of our
// windows has focus. Where the platform has a native reader (Windows), the clipboard's
// text is read with it instead of through Qt (see NativeClipboard.h).
class QtClipboardBackend : public CaptureBackend {
    Q_OBJECT

public:
    using NativeReader = std::function<NativeClipboardRead()>;

    // No `nativeReader`: readNativeClipboard() on the real Windows platform, Qt otherwise (the
    // offscreen/minimal platforms keep an in-process clipboard). Tests pass a fake one.
    QtClipboardBackend(QClipboard* clipboard, bool capturePrimary, QObject* parent = nullptr,
                       NativeReader nativeReader = {});

    QString name() const override { return QStringLiteral("qt-clipboard"); }
    void start() override;
    void stop() override;

    // A text that isn't ready yet is read again after 20, 40, 80, 160 and 320 ms, then skipped.
    static constexpr int kMaxRetries = 5;

private:
    void read(QClipboard::Mode mode);
    void readNative();

    QClipboard* m_clipboard;
    bool m_capturePrimary;
    NativeReader m_nativeReader;
    QTimer m_retry;
    int m_retries = 0;
    QMetaObject::Connection m_clipboardConn;
    QMetaObject::Connection m_selectionConn;
};

} // namespace clipitto::capture
