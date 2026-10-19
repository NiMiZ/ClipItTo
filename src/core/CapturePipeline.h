#pragma once

#include "ClipFilter.h"

#include <QObject>

namespace clipitto {

namespace db {
class Repository;
}

// Captured text or picture -> filter -> store (dedupe) -> prune. Shared by every capture backend.
class CapturePipeline : public QObject {
    Q_OBJECT

public:
    CapturePipeline(db::Repository& repo, int maxHistory, qint64 maxItemBytes, QObject* parent = nullptr);

    void setLimits(int maxHistory, qint64 maxItemBytes);
    // maxImages = 0 records no pictures at all.
    void setImageLimits(int maxImages, qint64 maxImageBytes);
    int maxImages() const { return m_maxImages; }
    ClipFilter& filter() { return m_filter; }

    // While paused (the database is busy elsewhere, e.g. being encrypted), accepted clips
    // wait in memory; resuming stores them in order.
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

public slots:
    void process(const QString& text, const QStringList& mimeTypes);
    void processImage(const clipitto::ImageClip& image, const QStringList& mimeTypes);

signals:
    void stored(qint64 id, bool inserted);
    void rejected(clipitto::ClipFilter::Verdict verdict);

private:
    void store(const QString& text);
    void storeImage(const ImageClip& image);

    db::Repository& m_repo;
    ClipFilter m_filter;
    int m_maxHistory;
    int m_maxImages = 100;
    bool m_paused = false;
    QList<QString> m_waiting;
    QList<ImageClip> m_waitingImages; // a few at most: pictures are big
};

} // namespace clipitto
