#pragma once

#include "CaptureBackend.h"
#include "NativeClipboard.h"

#include <QClipboard>
#include <QMimeData>
#include <QTimer>

#include <QDateTime>
#include <QImage>

#include <functional>
#include <optional>

namespace clipitto::capture {

// QClipboard change notifications. Complete on X11 (Qt uses XFixes selection events) and
// Windows (clipboard format listener); on Wayland, Qt only sees changes while one of our
// windows has focus. macOS sends a background program no change event at all (Qt reports
// changes only when the app becomes active), so there the pasteboard's change count is polled
// (see NativeClipboard.h). Where the platform has a native reader (Windows, macOS), the
// clipboard's text is read with it instead of through Qt.
class QtClipboardBackend : public CaptureBackend {
    Q_OBJECT

public:
    using NativeReader = std::function<NativeClipboardRead()>;
    // A number that changes with the clipboard's content, polled every kPollMs; -1: the
    // platform has a change event instead (QClipboard's signal is used).
    using ChangeCounter = std::function<qint64()>;

    // No `nativeReader`: readNativeClipboard() on the real Windows and macOS platforms, Qt
    // otherwise (the offscreen/minimal platforms keep an in-process clipboard); no
    // `changeCounter`: clipboardChangeCount() on macOS. Tests pass fakes.
    QtClipboardBackend(QClipboard* clipboard, bool capturePrimary, QObject* parent = nullptr,
                       NativeReader nativeReader = {}, ChangeCounter changeCounter = {});

    QString name() const override { return QStringLiteral("qt-clipboard"); }
    void start() override;
    void stop() override;

    // A text that isn't ready yet is read again after 20, 40, 80, 160 and 320 ms, then skipped.
    static constexpr int kMaxRetries = 5;
    // How often a change counter is read: cheap, and well under the 1 s own-write window.
    static constexpr int kPollMs = 250;

private:
    void read(QClipboard::Mode mode);
    void readNative();
    void poll(); // a changed counter means a new copy
    // Reports a text copy; a marked one with its source app, or without its text when
    // password copies aren't kept.
    void emitText(const QString& text, QStringList formats, const QString& html = QString());
    QString htmlOf(const QMimeData* data, const QString& text) const;
    void readImage(const QStringList& formats); // the clipboard's picture through Qt, if any

    // A picture waiting to be encoded: decoded already (Qt path) or as bytes (native path).
    struct PendingImage {
        QImage image;
        QByteArray data;
        QString type;
        QStringList formats;
        QDateTime copiedAt;
    };
    // One encode at a time; a newer picture replaces one still waiting.
    void encode(PendingImage pending);
    bool m_encoding = false;
    std::optional<PendingImage> m_waiting;

    QClipboard* m_clipboard;
    bool m_capturePrimary;
    NativeReader m_nativeReader;
    ChangeCounter m_changeCounter;
    QTimer m_poll;
    qint64 m_lastCount = -1;
    QTimer m_retry;
    int m_retries = 0;
    QMetaObject::Connection m_clipboardConn;
    QMetaObject::Connection m_selectionConn;
};

} // namespace clipitto::capture
