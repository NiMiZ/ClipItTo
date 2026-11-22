#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <atomic>

namespace clipitto::ui {

struct ThumbnailLimits {
    qint64 maxBytes = 50LL * 1024 * 1024; // file size, as max_image_bytes for pictures
    qint64 maxPixels = 80LL * 1000 * 1000; // same as capture::kMaxImagePixels
    int maxSide = 128; // longest side of a thumbnail
    int cacheSize = 64; // thumbnails (and remembered failures) kept
};

// Small thumbnails of picture files in a copied file list (the list rows and their hover preview).
//
// Rules: made lazily (only for a row that is painted), on a worker thread (the GUI thread never opens a file), from the
// file as it is when the row is shown (clear() when the popup opens), kept in memory only, in a small LRU, never written
// anywhere. Huge files are not decoded; a missing, moved, unreadable or non-picture file just has no thumbnail (the row keeps
// its icon). Nothing here logs a path or a file name: only counts and outcome classes.
class FileThumbnails : public QObject {
    Q_OBJECT

public:
    using Limits = ThumbnailLimits;
    enum class Outcome { Ok, Missing, NotAPicture, TooLarge, Network, Unreadable };
    static constexpr int kOutcomes = 6;

    explicit FileThumbnails(QObject* parent = nullptr, const Limits& limits = Limits());
    ~FileThumbnails() override;

    // True when the name's suffix is one of the picture formats Qt can read here (not SVG: it is drawn code, not a photo).
    static bool looksLikeImage(const QString& path);
    // The first of (the first few) paths that looks like a picture; empty when none does.
    static QString pictureAmong(const QStringList& paths);
    // The path is on another computer (`//server/x`, `\\server\x`, `smb:`, a gvfs mount) or, where Qt can tell, on a network
    // file system. Looks at the text only, except for the file-system type (a worker thread asks that).
    static bool isNetworkPath(const QString& path);
    // Synchronous, safe on any thread, and what the worker runs: reads the one file. `image` is set on Outcome::Ok only.
    static Outcome load(const QString& path, const Limits& limits, QImage* image);

    // The thumbnail when it is ready; a null pixmap otherwise (the row keeps its icon). The first call for a path starts
    // the work, and ready(path) is emitted when it is done and there is something to show.
    QPixmap thumbnail(const QString& path);
    // Forgets everything (and ignores work in flight): the next time a row is shown the file is looked at again.
    void clear();

    int cachedCount() const { return int(m_cache.size()); }
    int pendingCount() const { return int(m_inFlight.size()); }
    int outcomeCount(Outcome outcome) const { return m_counts[int(outcome)]; }
    const Limits& limits() const { return m_limits; }
    static const char* outcomeName(Outcome outcome);

signals:
    void ready(const QString& path);

private:
    struct Entry {
        QPixmap pixmap; // null: nothing to show for this file (remembered so it is not tried at every paint)
    };
    void finished(quint64 generation, const QString& path, Outcome outcome, const QImage& image);

    Limits m_limits;
    QCache<QString, Entry> m_cache;
    QHash<QString, quint64> m_inFlight;
    int m_counts[kOutcomes] = {};
    std::atomic<quint64> m_generation{1};
    QThreadPool m_pool;
};

} // namespace clipitto::ui
