#pragma once

#include <QSet>
#include "AutoGroup.h"
#include "Entry.h"
#include "ImageClip.h"
#include "PasswordVault.h"
#include "SearchFilter.h"

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
    // A password copy, already sealed by PasswordVault. `match` ("secret:<hex>") finds the same
    // password again (only last_used is bumped then); empty without the keyring's match key,
    // and then every copy is a new entry. The note and group apply to a new entry only.
    IngestResult ingestSecret(const QByteArray& sealed, const QString& match, const QString& note,
                              const QString& group);
    QByteArray sealedSecret(qint64 id); // empty for other copies
    std::vector<std::pair<qint64, QByteArray>> allSealedSecrets(); // newest first
    std::optional<Entry> findSecret(const QString& match);
    // "Mark as password": the text is replaced by the sealed copy, and wiped from the file
    // (secure_delete, search index, WAL). Returns the id that holds it: another one when the
    // same password was already kept (this copy is then removed).
    qint64 convertToSecret(qint64 id, const QByteArray& sealed, const QString& match);
    // "Not a password": the copy becomes plain text again. Returns the id that holds it.
    qint64 convertToText(qint64 id, const QString& text);
    qint64 countSecrets();

    // The master password's vault; nullopt until one is set.
    std::optional<PasswordVault::Stored> loadVault();
    void saveVault(const PasswordVault::Stored& stored);
    // A new key pair: every sealed password replaced and the new vault saved, in one
    // transaction; the old blobs are scrubbed from the file afterwards.
    void rekeyVault(const std::vector<std::pair<qint64, QByteArray>>& resealed, const PasswordVault::Stored& stored);
    // Removes the vault and every password copy (a forgotten or removed master password).
    void resetVault();
    // The vault was set up without a master password (a fixed key, so nothing is protected).
    bool vaultOpen();
    void setVaultOpen(bool open);

    static QString secretLabel() { return QStringLiteral("Password"); }

    // The full picture (PNG) of an image entry; empty for text or a missing id.
    QByteArray imageData(qint64 id);
    // The HTML flavour of a text copy with formatting (RichText.h), kept next to its plain text.
    // setRich replaces any earlier one; the copy must exist. richText is empty when there is none.
    void setRich(qint64 id, const QString& html);
    void clearRich(qint64 id);
    void clearAllRich();
    QString richText(qint64 id);

    // Search inside pictures: the text read from a picture (see clip_text). nextPictureToIndex is the newest
    // picture not read yet (0 = none); an empty text records "nothing found". clearPictureText forgets all of it.
    qint64 nextPictureToIndex();
    void setPictureText(qint64 id, const QString& text);
    void clearPictureText();
    qint64 countIndexedPictures();

    void setLimits(const Limits& limits) { m_limits = limits; }
    const Limits& limits() const { return m_limits; }

    // Newest first. With unsavedOnly, pinned, starred and grouped items are excluded; else the
    // filed copies that are on Recent (see takeOffRecent and the hidden-from-Recent groups).
    std::vector<Entry> recent(int limit, bool unsavedOnly);
    // The copy history: newest (last copied) first, each text once, leaving out pinned copies
    // (the popup lists those above). Filed copies that are on Recent are included.
    std::vector<Entry> recentUnpinned(int limit);
    // Takes a filed (starred, pinned or grouped) copy off Recent without unfiling it; copying
    // it again brings it back. Does nothing for an unfiled copy.
    void takeOffRecent(qint64 id);
    // What Recent lists now (unfiled copies, and filed ones that are on it), pinned ones aside:
    // those have their own list.
    qint64 countRecent();
    // Empties Recent: unfiled copies are deleted, starred and grouped ones only leave Recent
    // (pinned copies stay pinned). Returns the number of copies it listed.
    qint64 clearRecent();
    // Copies in a group (and its sub-groups) stay out of Recent. The same for starred copies
    // in no group (Favorites).
    void setGroupHiddenFromRecent(const QString& path, bool hidden);
    bool groupHiddenFromRecent(const QString& path);
    void setFavoritesHiddenFromRecent(bool hidden);
    bool favoritesHiddenFromRecent();
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

    enum class ContentChange { Done, Missing, NotText, Empty, TooLarge, Duplicate };
    // Replaces the text of a plain-text copy in place (one transaction): note, group, star, pin,
    // position and last_used stay; the search index and the duplicate hash follow the new text.
    // Refused (nothing changed) when empty, over `maxBytes` of UTF-8, or the same copy as another
    // one (same hash under the ignore-case choice). Pictures and passwords are NotText.
    ContentChange setContent(qint64 id, const QString& text, qint64 maxBytes);
    // The copy with the newest last_used (the top of Recent); nullopt for an empty history.
    std::optional<qint64> newestId();
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

    // --- Backup support
    // Every copy (pictures and passwords as rows without their data), oldest first.
    std::vector<Entry> allEntries();
    // The copy with this content_hash ("secret:<hex>", "image:<hash>", or hashOf(text)).
    std::optional<qint64> findByHash(const QString& contentHash);
    // Sets the times, note, group (created if new) and star of a copy as saved in a backup.
    // Not limited by the star caps: a backup restores what was there.
    void restoreFiling(qint64 id, const QDateTime& created, const QDateTime& lastUsed, const QString& note,
                       const QString& group, bool favorite, bool offRecent = false);
    // Ids of the filed copies that were taken off Recent.
    QSet<qint64> offRecentIds();
    // Puts the group last among its siblings (a restored group arriving after the existing ones).
    void placeGroupLast(const QString& path);

    qint64 count();
    qint64 countUnsaved();
    qint64 countImages();

    // PRAGMA optimize + FTS segment merge; cheap enough for shutdown or a weekly timer.
    void optimize();

    static QString hashOf(const QString& text);

    // With `ignore`, texts that differ only in letter case are one copy (the wording copied
    // last is kept). Changing it re-hashes the history once, merging copies that now match
    // into the most recently used one; the choice is remembered in the history file.
    void setIgnoreCase(bool ignore);
    bool ignoreCase() const { return m_ignoreCase; }
    // The hash that identifies `text` as a copy, under the current choice.
    QString textHash(const QString& text) const;

    // --- Auto grouping (AutoGroup.h). Learning happens in setGroup, only while `on` (auto_group).
    struct AutoExamples {
        QString group;
        int examples = 0;
    };
    void setAutoGrouping(bool on);
    bool autoGrouping() const { return m_autoGrouping; }
    // Counts the times what was learned was forgotten or a group deleted: an answer computed before is stale.
    quint64 autoForgets() const { return m_autoForgets; }
    // What was learned, as an immutable snapshot (cached until something changes); the first call
    // also reads the copies already in groups as examples. Safe to use from another thread.
    std::shared_ptr<const autogroup::Model> autoModel();
    // Files a copy in a group automatically (tagged "auto"). Only when the copy is a plain text one that is
    // still unsaved (no group, star or pin) and the group exists; never creates a group. True when filed.
    bool setAutoGroup(qint64 id, const QString& group);
    std::vector<AutoExamples> autoExamples(); // groups with examples, by name
    // "Forget what was learned": all examples gone; filed copies stay where they are.
    void forgetAutoLearned();

    // With `perNote`, the same password under a different note is another copy; otherwise it
    // is the same copy whatever its note.
    void setSecretsPerNote(bool perNote) { m_secretsPerNote = perNote; }
    // Whether a password copy with this match (and, per note, this note) is kept already.
    bool hasSecret(const QString& match, const QString& note);
    // Every password copy with this match, newest first.
    std::vector<Entry> findSecrets(const QString& match);

private:
    QString now(); // strictly increasing, so ordering by last_used never ties within a process
    std::vector<Entry> collect(Stmt& stmt);
    bool checkpoint(); // WAL folded in and truncated; false if a reader kept it busy
    void scrub(); // after removing text: search index merged, WAL folded in
    void loadImageDetails(Entry& entry);
    std::vector<Entry> searchLike(const QStringList& tokens, int limit);
    std::vector<Entry> searchPictureText(const QStringList& tokens, int limit);
    static std::vector<Entry> mergeNewest(std::vector<Entry> a, std::vector<Entry> b, int limit);
    std::vector<Entry> searchFiltered(const SearchFilter& filter, int limit);
    Stmt& likeStatement(int tokenCount);
    int starsIn(const QString& group); // "" = Favorites (starred, no group)
    Change roomForStar(const QString& group);
    void learn(const QString& group, const QString& text, int delta); // delta < 0 unlearns
    void ensureAutoSeeded();

    Db& m_db;
    Clock m_clock;
    Limits m_limits;
    bool m_ignoreCase = false;
    bool m_secretsPerNote = false;
    bool m_autoGrouping = false;
    quint64 m_autoForgets = 0;
    std::shared_ptr<const autogroup::Model> m_autoModel;
    QDateTime m_lastStamp;
    std::unique_ptr<Stmt> m_ingest;
    std::unique_ptr<Stmt> m_ingestImage;
    std::unique_ptr<Stmt> m_ingestSecret;
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
