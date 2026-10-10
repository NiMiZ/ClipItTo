#include "X11QtBackend.h"

#include <QMimeData>

namespace clipitto::capture {

X11QtBackend::X11QtBackend(QClipboard* clipboard, bool capturePrimary, QObject* parent)
    : CaptureBackend(parent)
    , m_clipboard(clipboard)
    , m_capturePrimary(capturePrimary)
{
}

void X11QtBackend::start()
{
    stop();
    m_clipboardConn = connect(m_clipboard, &QClipboard::dataChanged, this,
                              [this] { read(QClipboard::Clipboard); });
    if (m_capturePrimary && m_clipboard->supportsSelection()) {
        m_selectionConn = connect(m_clipboard, &QClipboard::selectionChanged, this,
                                  [this] { read(QClipboard::Selection); });
    }
}

void X11QtBackend::stop()
{
    disconnect(m_clipboardConn);
    disconnect(m_selectionConn);
}

void X11QtBackend::read(QClipboard::Mode mode)
{
    const QMimeData* data = m_clipboard->mimeData(mode);
    if (!data || !data->hasText()) {
        return;
    }
    emit captured(data->text(), data->formats());
}

} // namespace clipitto::capture
