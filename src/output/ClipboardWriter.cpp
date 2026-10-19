#include "ClipboardWriter.h"

#include "ClipFilter.h"
#include "DesktopIntegration.h"

#include <QClipboard>
#include <QImage>
#include <QMimeData>

namespace clipitto {

ClipboardWriter::ClipboardWriter(QClipboard* clipboard, ClipFilter& filter, DesktopIntegration* integration)
    : m_clipboard(clipboard)
    , m_filter(filter)
    , m_integration(integration)
{
}

void ClipboardWriter::setImage(const QByteArray& png, const QString& pixelHash)
{
    m_filter.noteOwnImage(pixelHash);
    if (m_integration && m_integration->isAvailable()) {
        m_integration->setClipboardImage(png);
        return;
    }
    auto* data = new QMimeData;
#if defined(_WIN32)
    // A bitmap (CF_DIB / CF_DIBV5) for the many apps that only take that, and the standard
    // "PNG" format, which keeps transparency for those that read it (Office, browsers).
    data->setImageData(QImage::fromData(png, "PNG"));
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"PNG\""), png);
#elif defined(__linux__)
    // X11 and Wayland apps ask for image/png: the stored bytes serve as they are, nothing to decode.
    data->setData(QStringLiteral("image/png"), png);
#else
#error "unsupported platform"
#endif
    m_clipboard->setMimeData(data, QClipboard::Clipboard);
}

void ClipboardWriter::setText(const QString& text)
{
    m_filter.noteOwnWrite(text);
    if (m_integration && m_integration->isAvailable()) {
        m_integration->setClipboard(text);
    } else {
        m_clipboard->setText(text, QClipboard::Clipboard);
    }
}

} // namespace clipitto
