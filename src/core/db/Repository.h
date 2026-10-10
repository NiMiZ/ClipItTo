#pragma once

#include "Entry.h"

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

    struct IngestResult {
        qint64 id;
        bool inserted; // false when an identical clip existed and only last_used was bumped
    };

    enum class SearchMode {
        Auto, // FTS5 trigram when every token has >= 3 characters, LIKE otherwise
        ForceLike, // for tests: always use the LIKE path
    };

    explicit Repository(Db& db, Clock clock = {});
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    IngestResult ingest(const QString& text);

    // Newest first. With unsavedOnly, favorites and grouped items are excluded.
    std::vector<Entry> recent(int limit, bool unsavedOnly);
    // Favorites first, then grouped items, each newest first.
    std::vector<Entry> saved(int limit);
    // Saved matches first, then by relevance (note > group > content), then recency.
    // Returns nothing for a blank query.
    std::vector<Entry> search(const QString& query, int limit = 100, SearchMode mode = SearchMode::Auto);

    std::optional<Entry> get(qint64 id);
    void touch(qint64 id); // last_used = now
    void setFavorite(qint64 id, bool favorite);
    void setGroup(qint64 id, const QString& group); // empty clears the group
    void setNote(qint64 id, const QString& note); // empty clears the note
    void remove(qint64 id);

    // Deletes the oldest unsaved entries so at most maxUnsaved remain. Returns the number deleted.
    qint64 prune(int maxUnsaved);

    QStringList groups();
    qint64 count();
    qint64 countUnsaved();

    // PRAGMA optimize + FTS segment merge; cheap enough for shutdown or a weekly timer.
    void optimize();

    static QString hashOf(const QString& text);

private:
    QString now(); // strictly increasing, so ordering by last_used never ties within a process
    std::vector<Entry> collect(Stmt& stmt);
    std::vector<Entry> searchLike(const QStringList& tokens, int limit);
    Stmt& likeStatement(int tokenCount);

    Db& m_db;
    Clock m_clock;
    QDateTime m_lastStamp;
    std::unique_ptr<Stmt> m_ingest;
    std::unique_ptr<Stmt> m_recentAll;
    std::unique_ptr<Stmt> m_recentUnsaved;
    std::unique_ptr<Stmt> m_saved;
    std::unique_ptr<Stmt> m_searchFts;
    std::unique_ptr<Stmt> m_get;
    std::unique_ptr<Stmt> m_touch;
    std::unique_ptr<Stmt> m_setFavorite;
    std::unique_ptr<Stmt> m_setGroup;
    std::unique_ptr<Stmt> m_setNote;
    std::unique_ptr<Stmt> m_remove;
    std::unique_ptr<Stmt> m_prune;
    std::unique_ptr<Stmt> m_groups;
    std::unique_ptr<Stmt> m_count;
    std::unique_ptr<Stmt> m_countUnsaved;
    std::unordered_map<int, std::unique_ptr<Stmt>> m_likeByTokenCount;
};

} // namespace clipitto::db
