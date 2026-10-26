#include "ClipboardWriter.h"

#include "ClipFilter.h"
#include "DesktopIntegration.h"
#if defined(__APPLE__)
#include "MacPasteboard.h"
#endif

#include <QClipboard>
#include <QGuiApplication>
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
#elif defined(__APPLE__)
    // Qt offers the picture to Cocoa as TIFF and PNG (public.tiff, public.png).
    data->setImageData(QImage::fromData(png, "PNG"));
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

void ClipboardWriter::clear()
{
    if (m_integration && m_integration->isAvailable()) {
        m_integration->setClipboard(QString());
    } else {
        m_clipboard->clear(QClipboard::Clipboard);
    }
}

void ClipboardWriter::setPassword(const QString& text)
{
    m_filter.noteOwnPassword(text);
    if (m_integration && m_integration->isAvailable()) {
        m_integration->setClipboard(text);
        return;
    }
#if defined(__APPLE__)
    // The real pasteboard, marked the way macOS password managers mark theirs. (Other Qt
    // platforms on a Mac, like the offscreen one the tests use, keep their own clipboard.)
    if (QGuiApplication::platformName() == QLatin1String("cocoa")) {
        setConcealedText(text);
        return;
    }
#endif
    auto* data = new QMimeData;
    data->setText(text);
#if defined(_WIN32)
    // The formats Windows' clipboard history and cloud clipboard honour (registered formats).
    const QByteArray zero(4, '\0'); // DWORD 0
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""),
                  zero);
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanIncludeInClipboardHistory\""), zero);
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanUploadToCloudClipboard\""), zero);
#elif defined(__linux__)
    data->setData(QStringLiteral("x-kde-passwordManagerHint"), QByteArrayLiteral("secret"));
#elif defined(__APPLE__)
    data->setData(QStringLiteral("org.nspasteboard.ConcealedType"), QByteArrayLiteral(" "));
#else
#error "unsupported platform"
#endif
    m_clipboard->setMimeData(data, QClipboard::Clipboard);
}

} // namespace clipitto
