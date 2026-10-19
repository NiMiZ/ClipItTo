#include "CapturePipeline.h"

#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QLoggingCategory>

#include <utility>

Q_LOGGING_CATEGORY(lcCapture, "clipitto.capture")

namespace clipitto {

CapturePipeline::CapturePipeline(db::Repository& repo, int maxHistory, qint64 maxItemBytes, QObject* parent)
    : QObject(parent)
    , m_repo(repo)
    , m_filter(maxItemBytes)
    , m_maxHistory(maxHistory)
{
}

void CapturePipeline::setLimits(int maxHistory, qint64 maxItemBytes)
{
    m_maxHistory = maxHistory;
    m_filter.setMaxBytes(maxItemBytes);
}

void CapturePipeline::setImageLimits(int maxImages, qint64 maxImageBytes)
{
    m_maxImages = qMax(0, maxImages);
    m_filter.setMaxImageBytes(maxImageBytes);
    if (m_maxImages == 0) {
        m_waitingImages.clear(); // turned off: don't store what was waiting either
    }
}

void CapturePipeline::setPaused(bool paused)
{
    m_paused = paused;
    if (paused) {
        return;
    }
    const QList<QString> waiting = std::exchange(m_waiting, {});
    for (const QString& text : waiting) {
        store(text);
    }
    const QList<ImageClip> images = std::exchange(m_waitingImages, {});
    for (const ImageClip& image : images) {
        storeImage(image);
    }
}

void CapturePipeline::processImage(const ImageClip& image, const QStringList& mimeTypes)
{
    if (m_maxImages == 0) {
        return; // pictures are turned off
    }
    const ClipFilter::Verdict verdict = m_filter.checkImage(image, mimeTypes);
    if (verdict != ClipFilter::Verdict::Accept) {
        qCDebug(lcCapture) << "skipped image, verdict" << int(verdict) << "bytes" << image.png.size();
        emit rejected(verdict);
        return;
    }
    if (m_paused) {
        constexpr qsizetype kMaxWaitingImages = 2; // pictures are big, and held unencrypted here
        m_waitingImages << image;
        while (m_waitingImages.size() > kMaxWaitingImages) {
            m_waitingImages.removeFirst();
        }
        return;
    }
    storeImage(image);
}

void CapturePipeline::storeImage(const ImageClip& image)
{
    try {
        const auto result = m_repo.ingestImage(image);
        if (result.inserted) {
            m_repo.pruneImages(m_maxImages);
        }
        emit stored(result.id, result.inserted);
    } catch (const db::DbError& e) {
        qCWarning(lcCapture) << "failed to store image:" << e.what();
    }
}

void CapturePipeline::process(const QString& text, const QStringList& mimeTypes)
{
    const ClipFilter::Verdict verdict = m_filter.check(text, mimeTypes);
    if (verdict != ClipFilter::Verdict::Accept) {
        qCDebug(lcCapture) << "skipped clip, verdict" << int(verdict);
        emit rejected(verdict);
        return;
    }
    if (m_paused) {
        m_waiting.removeAll(text); // the same text copied twice is one entry anyway
        m_waiting << text;
        while (m_waiting.size() > m_maxHistory) {
            m_waiting.removeFirst(); // keep memory bounded: the oldest would be pruned anyway
        }
        return;
    }
    store(text);
}

void CapturePipeline::store(const QString& text)
{
    try {
        const auto result = m_repo.ingest(text);
        if (result.inserted) {
            // The prune query is index-backed and cheap, so the limit is kept exact.
            m_repo.prune(m_maxHistory);
        }
        emit stored(result.id, result.inserted);
    } catch (const db::DbError& e) {
        qCWarning(lcCapture) << "failed to store clip:" << e.what();
    }
}

} // namespace clipitto
