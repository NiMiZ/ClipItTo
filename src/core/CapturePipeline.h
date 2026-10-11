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

    // While paused (the database is busy elsewhere, e.g. being encrypted), accepted clips
    // wait in memory; resuming stores them in order.
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

public slots:
    void process(const QString& text, const QStringList& mimeTypes);

signals:
    void stored(qint64 id, bool inserted);
    void rejected(clipitto::ClipFilter::Verdict verdict);

private:
    void store(const QString& text);

    db::Repository& m_repo;
    ClipFilter m_filter;
    int m_maxHistory;
    bool m_paused = false;
    QList<QString> m_waiting;
};

} // namespace clipitto
