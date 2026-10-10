#include "QtClipboardBackend.h"

#include <QMimeData>

namespace clipitto::capture {

QtClipboardBackend::QtClipboardBackend(QClipboard* clipboard, bool capturePrimary, QObject* parent)
    : CaptureBackend(parent)
    , m_clipboard(clipboard)
    , m_capturePrimary(capturePrimary)
{
}

void QtClipboardBackend::start()
{
    stop();
    m_clipboardConn = connect(m_clipboard, &QClipboard::dataChanged, this,
                              [this] { read(QClipboard::Clipboard); });
    if (m_capturePrimary && m_clipboard->supportsSelection()) {
        m_selectionConn = connect(m_clipboard, &QClipboard::selectionChanged, this,
                                  [this] { read(QClipboard::Selection); });
    }
}

void QtClipboardBackend::stop()
{
    disconnect(m_clipboardConn);
    disconnect(m_selectionConn);
}

void QtClipboardBackend::read(QClipboard::Mode mode)
{
    const QMimeData* data = m_clipboard->mimeData(mode);
    if (!data || !data->hasText()) {
        return;
    }
    emit captured(data->text(), data->formats());
}

} // namespace clipitto::capture
