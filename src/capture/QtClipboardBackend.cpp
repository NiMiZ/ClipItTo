#include "QtClipboardBackend.h"

#include "ClipFilter.h"
#include "ImageCodec.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMimeData>

#include <utility>

Q_LOGGING_CATEGORY(lcQtClipboard, "clipitto.capture.qt")

namespace clipitto::capture {

QtClipboardBackend::QtClipboardBackend(QClipboard* clipboard, bool capturePrimary, QObject* parent,
                                       NativeReader nativeReader)
    : CaptureBackend(parent)
    , m_clipboard(clipboard)
    , m_capturePrimary(capturePrimary)
    , m_nativeReader(std::move(nativeReader))
{
    if (!m_nativeReader && QGuiApplication::platformName() == QLatin1String("windows")) {
        m_nativeReader = readNativeClipboard;
    }
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &QtClipboardBackend::readNative);
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
    m_retry.stop();
}

void QtClipboardBackend::read(QClipboard::Mode mode)
{
    if (mode == QClipboard::Clipboard && m_nativeReader) {
        m_retries = 0; // a new copy replaces one still being retried
        m_retry.stop();
        readNative();
        return;
    }
    const QMimeData* data = m_clipboard->mimeData(mode);
    if (!data) {
        return;
    }
    if (data->hasText()) {
        emit captured(data->text(), data->formats());
    } else if (mode == QClipboard::Clipboard && data->hasImage()) {
        readImage(data->formats()); // text wins when a copy offers both
    }
}

void QtClipboardBackend::readImage(const QStringList& formats)
{
    if (ClipFilter::isSensitive(formats)) {
        return; // never read what a password manager marked
    }
    const QMimeData* data = m_clipboard->mimeData(QClipboard::Clipboard);
    // Checked again on what is read now: on Windows this is a second look at the clipboard
    // after the native read, and it may have changed in between.
    if (!data || !data->hasImage() || ClipFilter::isSensitive(data->formats())) {
        return;
    }
    if (m_clipboard->ownsClipboard()) {
        return; // our own paste: its echo is ignored anyway, no need to encode it
    }
    // X11/Wayland: the PNG as the source sent it, decoded off the GUI thread. Otherwise the
    // picture as Qt converts it.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (data->hasFormat(QStringLiteral("image/png"))) {
        encode({QImage(), data->data(QStringLiteral("image/png")), QStringLiteral("image/png"), formats, now});
    } else {
        encode({qvariant_cast<QImage>(data->imageData()), QByteArray(), QString(), formats, now});
    }
}

void QtClipboardBackend::encode(PendingImage pending)
{
    if (m_encoding) {
        m_waiting = std::move(pending);
        return;
    }
    m_encoding = true;
    auto done = [this, formats = pending.formats, copiedAt = pending.copiedAt](std::optional<ImageClip> clip) {
        m_encoding = false;
        if (clip) {
            clip->copiedAt = copiedAt;
            emit capturedImage(*clip, formats);
        }
        if (m_waiting) {
            encode(*std::exchange(m_waiting, std::nullopt));
        }
    };
    if (pending.data.isEmpty()) {
        encodeImageAsync(pending.image, maxImageBytes(), this, std::move(done));
    } else {
        encodeImageDataAsync(pending.data, pending.type, maxImageBytes(), this, std::move(done));
    }
}

void QtClipboardBackend::readNative()
{
    const NativeClipboardRead result = m_nativeReader();
    switch (result.status) {
    case NativeClipboardRead::Status::Unsupported: {
        m_nativeReader = nullptr; // this platform has none: Qt from now on
        read(QClipboard::Clipboard);
        return;
    }
    case NativeClipboardRead::Status::Ok:
        if (!result.image.isEmpty()) {
            // A picture, read with the markers checked in the same clipboard session.
            if (!m_clipboard->ownsClipboard()) {
                encode({QImage(), result.image, result.imageType, result.formats, QDateTime::currentDateTimeUtc()});
            }
            return;
        }
        emit captured(result.text, result.formats);
        return;
    case NativeClipboardRead::Status::NoText:
        return;
    case NativeClipboardRead::Status::Busy:
        if (m_retries < kMaxRetries) {
            m_retry.start(20 << m_retries);
            ++m_retries;
        } else {
            qCWarning(lcQtClipboard) << "clipboard text still unavailable after" << kMaxRetries
                                     << "retries; this copy is not recorded";
        }
        return;
    }
}

} // namespace clipitto::capture
