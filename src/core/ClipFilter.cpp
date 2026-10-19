#include "ClipFilter.h"

#include <QElapsedTimer>

#include <algorithm>
#include <memory>

namespace clipitto {

namespace {

ClipFilter::MsClock monotonicClock()
{
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    return [timer] { return timer->elapsed(); };
}

} // namespace

ClipFilter::ClipFilter(qint64 maxBytes, MsClock clock)
    : m_maxBytes(maxBytes)
    , m_clock(clock ? std::move(clock) : monotonicClock())
{
}

bool ClipFilter::isSensitive(const QStringList& mimeTypes)
{
    static const QStringList markers = {
        QStringLiteral("x-kde-passwordManagerHint"),
        QStringLiteral("application/x-kde-passwordManagerHint"),
        QStringLiteral("application/x-nspasteboard-concealed-type"),
        QStringLiteral("ExcludeClipboardContentFromMonitorProcessing"),
        // Windows DWORD markers set to 0, as NativeClipboard reports them.
        QStringLiteral("CanIncludeInClipboardHistory=0"),
        QStringLiteral("CanUploadToCloudClipboard=0"),
        QStringLiteral("Clipboard Viewer Ignore"), // set by KeePass and others with the above
    };
    // Qt reports Windows formats it has no MIME type for wrapped:
    // application/x-qt-windows-mime;value="ExcludeClipboardContentFromMonitorProcessing"
    static const QString qtWrapped = QStringLiteral("application/x-qt-windows-mime;value=\"");
    return std::any_of(mimeTypes.begin(), mimeTypes.end(), [](const QString& type) {
        if (type.startsWith(qtWrapped, Qt::CaseInsensitive) && type.endsWith(QLatin1Char('"'))) {
            return markers.contains(type.mid(qtWrapped.size()).chopped(1), Qt::CaseInsensitive);
        }
        return markers.contains(type, Qt::CaseInsensitive);
    });
}

ClipFilter::Verdict ClipFilter::check(const QString& text, const QStringList& mimeTypes) const
{
    if (isSensitive(mimeTypes)) {
        return Verdict::Sensitive;
    }
    if (text.trimmed().isEmpty()) {
        return Verdict::Empty;
    }
    // UTF-8 is at most 3 bytes per UTF-16 unit, so only measure exactly when it could matter.
    if (text.size() * 3 > m_maxBytes && text.toUtf8().size() > m_maxBytes) {
        return Verdict::TooLarge;
    }
    if (text == m_ownText && isOwnEcho(m_hasOwn, m_ownAtMs)) {
        return Verdict::OwnWrite;
    }
    return Verdict::Accept;
}

ClipFilter::Verdict ClipFilter::checkImage(const ImageClip& image, const QStringList& mimeTypes) const
{
    if (isSensitive(mimeTypes)) {
        return Verdict::Sensitive;
    }
    if (!image.isValid()) {
        return Verdict::Empty;
    }
    if (image.png.size() > m_maxImageBytes) {
        return Verdict::TooLarge;
    }
    if (image.pixelHash == m_ownImageHash && isOwnEcho(m_hasOwnImage, m_ownImageAtMs)) {
        return Verdict::OwnWrite;
    }
    return Verdict::Accept;
}

bool ClipFilter::isOwnEcho(bool has, qint64 atMs) const
{
    return has && m_clock() - atMs <= ownWriteWindowMs;
}

void ClipFilter::noteOwnWrite(const QString& text)
{
    m_ownText = text;
    m_ownAtMs = m_clock();
    m_hasOwn = true;
}

void ClipFilter::noteOwnImage(const QString& pixelHash)
{
    m_ownImageHash = pixelHash;
    m_ownImageAtMs = m_clock();
    m_hasOwnImage = true;
}

} // namespace clipitto
