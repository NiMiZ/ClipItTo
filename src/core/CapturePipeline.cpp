#include "CapturePipeline.h"

#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QLoggingCategory>

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

void CapturePipeline::process(const QString& text, const QStringList& mimeTypes)
{
    const ClipFilter::Verdict verdict = m_filter.check(text, mimeTypes);
    if (verdict != ClipFilter::Verdict::Accept) {
        qCDebug(lcCapture) << "skipped clip, verdict" << int(verdict);
        emit rejected(verdict);
        return;
    }
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
