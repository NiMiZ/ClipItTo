#pragma once

#include "ClipFilter.h"

#include <QObject>

namespace clipitto {

namespace db {
class Repository;
}

// Captured text -> filter -> store (dedupe) -> prune. Shared by every capture backend.
class CapturePipeline : public QObject {
    Q_OBJECT

public:
    CapturePipeline(db::Repository& repo, int maxHistory, qint64 maxItemBytes, QObject* parent = nullptr);

    void setLimits(int maxHistory, qint64 maxItemBytes);
    ClipFilter& filter() { return m_filter; }

public slots:
    void process(const QString& text, const QStringList& mimeTypes);

signals:
    void stored(qint64 id, bool inserted);
    void rejected(clipitto::ClipFilter::Verdict verdict);

private:
    db::Repository& m_repo;
    ClipFilter m_filter;
    int m_maxHistory;
};

} // namespace clipitto
