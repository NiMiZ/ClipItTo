#pragma once

#include "Entry.h"
#include "ImageClip.h"

#include <QDateTime>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace clipitto::db {

class Db;
class Stmt;

// All clipboard-history queries. Statements are prepared once and reused.
// Expects a connection that has been configured and migrated.
class Repository {
public:
    using Clock = std::function<QDateTime()>;

    struct GroupInfo {
        QString path; // "Work/Servers"
        int clipCount = 0; // clips directly in this group (not in its sub-groups)
    };

    struct IngestResult {
        qint64 id;
        bool inserted; // false when an identical clip existed and only last_used was bumped
    };

    // Caps on stars and pins. They only block adding more: existing data over a lowered cap
    // (e.g. favorites_limit reduced in the settings) is kept.
    struct Limits {
        int favorites = 10; // starred copies without a group (the Favorites group)
        int starsPerGroup = 10; // starred copies directly in one group
        int pins = 10;
    };

    // Outcome of a change that a limit can refuse; nothing changed unless Done.
    enum class Change { Done, FavoritesFull, GroupStarsFull, PinsFull };

    enum class SearchMode {
        Auto, // FTS5 trigram when every token has >= 3 characters, LIKE otherwise
        ForceLike, // for tests: always use the LIKE path
    };

    explicit Repository(Db& db, Clock clock = {});
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    IngestResult ingest(const QString& text);
    // The same picture (same pixels) again only bumps its last_used, like text.
    IngestResult ingestImage(const ImageClip& image);
    // The full picture (PNG) of an image entry; empty for text or a missing id.
    QByteArray imageData(qint64 id);

    void setLimits(const Limits& limits) { m_limits = limits; }
    const Limits& limits() const { return m_limits; }

    // Newest first. With unsavedOnly, pinned, starred and grouped items are excluded.
    std::vector<Entry> recent(int limit, bool unsavedOnly);
    // The copy history: newest (last copied or pasted) first, each text once, leaving out
    // pinned copies (the popup lists those above). Starred and grouped copies are included.
    std::vector<Entry> recentUnpinned(int limit);
    // Pinned copies in their pinned order.
    std::vector<Entry> pinned(int limit);
    // Starred copies without a group (the Favorites group), newest first.
    std::vector<Entry> favorites();
    // Saved matches first, then by relevance (note > group > content), then recency.
    // Returns nothing for a blank query.
    std::vector<Entry> search(const QString& query, int limit = 100, SearchMode mode = SearchMode::Auto);

    std::optional<Entry> get(qint64 id);
    void touch(qint64 id); // last_used = now
    // Starring counts against the Favorites limit (no group) or the clip's group's limit.
    Change setFavorite(qint64 id, bool favorite);
    // A group path, created if new; empty clears. A starred clip keeps its star, so the
    // target (a group, or Favorites when clearing) must have room for it.
    Change setGroup(qint64 id, const QString& group);
    Change setPinned(qint64 id, bool pinned); // new pins go last
    bool movePinned(qint64 id, int delta); // -1 = up one place; false at either end
    void setNote(qint64 id, const QString& note); // empty clears the note
    void remove(qint64 id);
    // Deletes the history: unsaved copies (text and pictures) only, or with `everything` also
    // starred, pinned and grouped ones. Groups themselves stay. Returns the number deleted.
    qint64 clearHistory(bool everything);

    // Deletes the oldest unsaved text entries so at most maxUnsaved remain. Returns the number
    // deleted. Images have their own limit (pruneImages).
    qint64 prune(int maxUnsaved);
    // The same for images: at most maxUnsaved unsaved images remain.
    qint64 pruneImages(int maxUnsaved);

    // --- Groups: a tree of paths (see GroupPath.h). Parents of a group always exist.
    // Every group path, depth first: parents before children, siblings in the user's order
    // (moveGroup), then by name.
    QStringList groups();
    std::vector<GroupInfo> groupTree(); // same order, with direct clip counts
    std::vector<Entry> groupedEntries(); // all clips that are in a group; starred, then newest first
    // Moves a group among its siblings (-1 = up one place). False at either end.
    bool moveGroup(const QString& path, int delta);
    QString createGroup(const QString& path); // creates it and its parents; returns the normalized path
    // Renames or moves a group with everything below it. Fails if the target exists or
    // lies inside the group being moved.
    bool renameGroup(const QString& from, const QString& to, QString* error = nullptr);
    // Deletes a group and its sub-groups; their clips stay in the history, ungrouped (starred
    // ones go to Favorites even beyond its limit: limits only block adding more).
    void deleteGroup(const QString& path);

    qint64 count();
    qint64 countUnsaved();
    qint64 countImages();

    // PRAGMA optimize + FTS segment merge; cheap enough for shutdown or a weekly timer.
    void optimize();

    static QString hashOf(const QString& text);

private:
    QString now(); // strictly increasing, so ordering by last_used never ties within a process
    std::vector<Entry> collect(Stmt& stmt);
    bool checkpoint(); // WAL folded in and truncated; false if a reader kept it busy
    void loadImageDetails(Entry& entry);
    std::vector<Entry> searchLike(const QStringList& tokens, int limit);
    Stmt& likeStatement(int tokenCount);
    int starsIn(const QString& group); // "" = Favorites (starred, no group)
    Change roomForStar(const QString& group);

    Db& m_db;
    Clock m_clock;
    Limits m_limits;
    QDateTime m_lastStamp;
    std::unique_ptr<Stmt> m_ingest;
    std::unique_ptr<Stmt> m_ingestImage;
    std::unique_ptr<Stmt> m_imageDetails;
    std::unique_ptr<Stmt> m_recentAll;
    std::unique_ptr<Stmt> m_recentUnsaved;
    std::unique_ptr<Stmt> m_recentUnpinned;
    std::unique_ptr<Stmt> m_pinned;
    std::unique_ptr<Stmt> m_searchFts;
    std::unique_ptr<Stmt> m_get;
    std::unique_ptr<Stmt> m_touch;
    std::unique_ptr<Stmt> m_setFavorite;
    std::unique_ptr<Stmt> m_setGroup;
    std::unique_ptr<Stmt> m_setNote;
    std::unique_ptr<Stmt> m_remove;
    std::unique_ptr<Stmt> m_prune;
    std::unique_ptr<Stmt> m_count;
    std::unique_ptr<Stmt> m_countUnsaved;
    std::unordered_map<int, std::unique_ptr<Stmt>> m_likeByTokenCount;
};

} // namespace clipitto::db
