#pragma once

#include "CaptureBackend.h"

#include <QClipboard>

namespace clipitto::capture {

// QClipboard change notifications. Complete on X11 (Qt uses XFixes selection events);
// on Wayland, Qt only sees changes while one of our windows has focus.
class X11QtBackend : public CaptureBackend {
    Q_OBJECT

public:
    X11QtBackend(QClipboard* clipboard, bool capturePrimary, QObject* parent = nullptr);

    QString name() const override { return QStringLiteral("x11"); }
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
