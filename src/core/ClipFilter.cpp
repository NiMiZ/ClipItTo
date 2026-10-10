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
    };
    return std::any_of(mimeTypes.begin(), mimeTypes.end(),
                       [](const QString& type) { return markers.contains(type, Qt::CaseInsensitive); });
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
    if (m_hasOwn && text == m_ownText && m_clock() - m_ownAtMs <= ownWriteWindowMs) {
        return Verdict::OwnWrite;
    }
    return Verdict::Accept;
}

void ClipFilter::noteOwnWrite(const QString& text)
{
    m_ownText = text;
    m_ownAtMs = m_clock();
    m_hasOwn = true;
}

} // namespace clipitto
