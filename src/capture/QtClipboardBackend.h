#pragma once

#include "CaptureBackend.h"

#include <QClipboard>

namespace clipitto::capture {

// QClipboard change notifications. Complete on X11 (Qt uses XFixes selection events) and
// Windows (clipboard format listener); on Wayland, Qt only sees changes while one of our
// windows has focus.
class QtClipboardBackend : public CaptureBackend {
    Q_OBJECT

public:
    QtClipboardBackend(QClipboard* clipboard, bool capturePrimary, QObject* parent = nullptr);

    QString name() const override { return QStringLiteral("qt-clipboard"); }
    void start() override;
    void stop() override;

private:
    void read(QClipboard::Mode mode);

    QClipboard* m_clipboard;
    bool m_capturePrimary;
    QMetaObject::Connection m_clipboardConn;
    QMetaObject::Connection m_selectionConn;
};

} // namespace clipitto::capture
