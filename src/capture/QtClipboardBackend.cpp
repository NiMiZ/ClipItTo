#include "QtClipboardBackend.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMimeData>

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
    if (!data || !data->hasText()) {
        return;
    }
    emit captured(data->text(), data->formats());
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
