#include "db/Repository.h"

#include "AutoGroup.h"
#include "GroupPath.h"
#include "SecretScan.h"
#include "db/FtsQuery.h"
#include "db/Sqlite.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QHash>

#include <algorithm>
#include <map>

namespace clipitto::db {

namespace {

void setMeta(Db& db, const char* key, const QString& value);

// Column order read by readEntry().
// The condition for a copy (alias c) to be listed in Recent.
#define IN_RECENT                                                                                      \
    "(c.is_saved = 0 OR (c.off_recent = 0 AND NOT CASE "                                                \
    "WHEN c.group_name IS NOT NULL AND c.group_name <> '' THEN EXISTS (SELECT 1 FROM groups g "         \
    "WHERE g.hide_from_recent = 1 AND (c.group_name = g.path "                                          \
    "OR substr(c.group_name, 1, length(g.path) + 1) = g.path || '/')) "                                 \
    "ELSE c.is_favorite = 1 AND EXISTS (SELECT 1 FROM meta "                                            \
    "WHERE key = 'favorites_hide_from_recent' AND value = '1') END))"

#define CLIP_COLUMNS                                                                                   \
    "c.id, c.content, c.tag_or_note, c.group_name, c.is_favorite, c.created_at, c.last_used, c.pin_position, " \
    "c.kind, c.content_hash, c.secret, EXISTS (SELECT 1 FROM clip_rich r WHERE r.clip_id = c.id), " \
    "c.auto_filed, c.paste_once"

Entry readEntry(const Stmt& s)
{
    Entry e;
    e.id = s.colInt(0);
    e.content = s.colText(1);
    e.note = s.colText(2);
    e.group = s.colText(3);
    e.favorite = s.colInt(4) != 0;
    e.createdAt = QDateTime::fromString(s.colText(5), Qt::ISODateWithMs);
    e.lastUsed = QDateTime::fromString(s.colText(6), Qt::ISODateWithMs);
    e.pinPosition = s.colIsNull(7) ? -1 : int(s.colInt(7));
    e.image = s.colText(8) == QLatin1String("image");
    e.contentHash = s.colText(9);
    e.secret = s.colInt(10) != 0;
    e.rich = s.colInt(11) != 0;
    e.autoFiled = s.colInt(12) != 0;
    e.pasteOnce = s.colInt(13) != 0;
    return e;
}

bool looksSecret(const QString& text)
{
    return !secretscan::kindOf(text).isEmpty() || secretscan::looksLikeGeneratedPassword(text);
}

// A transaction that may run inside another one (a savepoint).
class Nested {
public:
    explicit Nested(Db& db) : m_db(db) { m_db.exec("SAVEPOINT auto_group"); }
    ~Nested()
    {
        if (!m_done) {
            try {
                m_db.exec("ROLLBACK TO auto_group");
                m_db.exec("RELEASE auto_group");
            } catch (const DbError&) {
            }
        }
    }
    Nested(const Nested&) = delete;
    Nested& operator=(const Nested&) = delete;
    void commit()
    {
        m_db.exec("RELEASE auto_group");
        m_done = true;
    }

private:
    Db& m_db;
    bool m_done = false;
};

} // namespace

Repository::Repository(Db& db, Clock clock)
    : m_db(db)
    , m_clock(clock ? std::move(clock) : Clock([] { return QDateTime::currentDateTimeUtc(); }))
{
    // Deleting a copy then also removes its text from the search index right away, not only
    // at the next merge (SQLite 3.42+; older versions merge on clear and at shutdown).
    try {
        m_db.exec("INSERT INTO clips_fts(clips_fts, rank) VALUES ('secure-delete', 1);");
    } catch (const DbError& e) {
        qWarning() << "search index secure-delete unavailable (SQLite before 3.42):" << e.what();
    }

    // created_at = last_used only holds right after an insert, which tells us whether the upsert inserted.
    m_ingest = std::make_unique<Stmt>(m_db, R"(
        INSERT INTO clips(content, content_hash, created_at, last_used) VALUES (?1, ?2, ?3, ?3)
        ON CONFLICT(content_hash) DO UPDATE SET last_used = excluded.last_used, off_recent = 0,
            content = excluded.content
        RETURNING id, created_at = last_used)");
    m_ingestImage = std::make_unique<Stmt>(m_db, R"(
        INSERT INTO clips(content, content_hash, created_at, last_used, kind) VALUES (?1, ?2, ?3, ?3, 'image')
        ON CONFLICT(content_hash) DO UPDATE SET last_used = excluded.last_used, off_recent = 0
        RETURNING id, created_at = last_used)");
    m_ingestSecret = std::make_unique<Stmt>(m_db, R"(
        INSERT INTO clips(content, content_hash, created_at, last_used, secret) VALUES (?1, ?2, ?3, ?3, 1)
        ON CONFLICT(content_hash) DO UPDATE SET last_used = excluded.last_used, off_recent = 0
        RETURNING id, created_at = last_used)");
    m_imageDetails = std::make_unique<Stmt>(m_db, "SELECT width, height, thumbnail FROM clip_images WHERE clip_id = ?1");

    // Recent shows every unfiled copy, and a filed one unless it was taken off Recent or its
    // group (Favorites, for a starred copy in none) is kept out of it. A copy in a group is
    // hidden by that group or any group above it.
    // Written as two index-friendly parts: the newest unfiled copies (the (is_saved, last_used) index reads just
    // those), and the filed ones (few) that Recent keeps. A single "is_saved = 0 OR ..." condition could not use an
    // index and read and sorted the whole history on every refresh (50k copies: ~190 ms, on the GUI thread).
    m_recentAll = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS " FROM clips c WHERE c.id IN ("
                                               "SELECT id FROM (SELECT id FROM clips WHERE is_saved = 0 ORDER BY last_used DESC, id DESC LIMIT ?1) "
                                               "UNION ALL SELECT c.id FROM clips c WHERE c.is_saved = 1 AND " IN_RECENT ")"
                                               " ORDER BY c.last_used DESC, c.id DESC LIMIT ?1");

    m_recentUnsaved = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS R"( FROM clips c
        WHERE c.is_saved = 0
        ORDER BY c.last_used DESC, c.id DESC LIMIT ?1)");

    m_recentUnpinned = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS " FROM clips c WHERE c.pin_position IS NULL AND c.id IN ("
                                                    "SELECT id FROM (SELECT id FROM clips WHERE is_saved = 0 AND pin_position IS NULL "
                                                    "ORDER BY last_used DESC, id DESC LIMIT ?1) "
                                                    "UNION ALL SELECT c.id FROM clips c WHERE c.is_saved = 1 AND " IN_RECENT ")"
                                                    " ORDER BY c.last_used DESC, c.id DESC LIMIT ?1");

    m_pinned = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS R"( FROM clips c
        WHERE c.pin_position IS NOT NULL
        ORDER BY c.pin_position, c.id LIMIT ?1)");

    // bm25 column weights: note 10, group 5, content 1 (lower score = better match).
    m_searchFts = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS R"(,
            bm25(clips_fts, 10.0, 5.0, 1.0) AS score
        FROM clips_fts JOIN clips c ON c.id = clips_fts.rowid
        WHERE clips_fts MATCH ?1
        ORDER BY c.last_used DESC, c.id DESC
        LIMIT ?2)");

    m_get = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS " FROM clips c WHERE c.id = ?1");
    m_touch = std::make_unique<Stmt>(m_db, "UPDATE clips SET last_used = ?2 WHERE id = ?1");
    m_setFavorite = std::make_unique<Stmt>(m_db, "UPDATE clips SET is_favorite = ?2 WHERE id = ?1");
    m_setGroup = std::make_unique<Stmt>(m_db, "UPDATE clips SET group_name = ?2 WHERE id = ?1");
    m_setNote = std::make_unique<Stmt>(m_db, "UPDATE clips SET tag_or_note = ?2 WHERE id = ?1");
    m_remove = std::make_unique<Stmt>(m_db, "DELETE FROM clips WHERE id = ?1");

    m_prune = std::make_unique<Stmt>(m_db, R"(
        DELETE FROM clips WHERE id IN (
            SELECT id FROM clips WHERE is_saved = 0 AND kind = 'text'
            ORDER BY last_used ASC, id ASC
            LIMIT max(0, (SELECT count(*) FROM clips WHERE is_saved = 0 AND kind = 'text') - ?1)))");

    m_count = std::make_unique<Stmt>(m_db, "SELECT count(*) FROM clips");
    m_countUnsaved = std::make_unique<Stmt>(m_db, "SELECT count(*) FROM clips WHERE is_saved = 0");
    Stmt mode(m_db, "SELECT value FROM meta WHERE key = 'ignore_case'");
    m_ignoreCase = mode.step() && mode.colText(0) == QLatin1String("1");
}

Repository::~Repository() = default;

QString Repository::now()
{
    // Timestamps have millisecond resolution; two writes in the same millisecond (e.g. a
    // copy immediately followed by re-using an older entry) must still order correctly.
    QDateTime stamp = m_clock().toUTC();
    if (m_lastStamp.isValid() && stamp <= m_lastStamp) {
        stamp = m_lastStamp.addMSecs(1);
    }
    m_lastStamp = stamp;
    return stamp.toString(Qt::ISODateWithMs);
}

QString Repository::hashOf(const QString& text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString Repository::textHash(const QString& text) const
{
    return hashOf(m_ignoreCase ? text.toCaseFolded() : text);
}

void Repository::setIgnoreCase(bool ignore)
{
    if (ignore == m_ignoreCase) {
        return;
    }
    Transaction tx(m_db);
    // Newest first, so that of copies that now match the most recently used one is kept.
    struct Row {
        qint64 id;
        QString text;
    };
    std::vector<Row> rows;
    {
        Stmt s(m_db, "SELECT id, content FROM clips WHERE kind = 'text' AND secret = 0 "
                     "ORDER BY last_used DESC, id DESC");
        while (s.step()) {
            rows.push_back({s.colInt(0), s.colText(1)});
        }
    }
    // Free every hash first: a copy's new hash may be the current one of another copy.
    m_db.exec("UPDATE clips SET content_hash = 'rehash:' || id WHERE kind = 'text' AND secret = 0;");
    m_ignoreCase = ignore;
    std::map<QString, qint64> owners; // new hash -> the copy that has it
    for (const Row& row : rows) {
        const QString hash = textHash(row.text);
        const auto owner = owners.find(hash);
        if (owner == owners.end()) {
            Stmt set(m_db, "UPDATE clips SET content_hash = ?2 WHERE id = ?1");
            set.bindInt(1, row.id).bindText(2, hash).run();
            owners.emplace(hash, row.id);
            continue;
        }
        // An older wording of the same text: its filing moves to the kept copy, which keeps
        // what it has itself.
        Stmt pin(m_db, "SELECT pin_position FROM clips WHERE id = ?1");
        pin.bindInt(1, row.id);
        const bool pinned = pin.step() && !pin.colIsNull(0);
        const qint64 pinPosition = pinned ? pin.colInt(0) : 0;
        pin.reset();
        Stmt merge(m_db, R"(UPDATE clips SET
                tag_or_note = COALESCE(tag_or_note, (SELECT tag_or_note FROM clips WHERE id = ?2)),
                group_name = COALESCE(group_name, (SELECT group_name FROM clips WHERE id = ?2)),
                is_favorite = MAX(is_favorite, (SELECT is_favorite FROM clips WHERE id = ?2))
            WHERE id = ?1)");
        merge.bindInt(1, owner->second).bindInt(2, row.id).run();
        remove(row.id);
        if (pinned) {
            Stmt setPin(m_db, "UPDATE clips SET pin_position = ?2 WHERE id = ?1 AND pin_position IS NULL");
            setPin.bindInt(1, owner->second).bindInt(2, pinPosition).run();
        }
    }
    Stmt save(m_db, "INSERT INTO meta(key, value) VALUES ('ignore_case', ?1) "
                    "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    save.bindText(1, ignore ? QStringLiteral("1") : QStringLiteral("0")).run();
    m_db.exec("INSERT INTO clips_fts(clips_fts) VALUES ('optimize');");
    tx.commit();
}

Repository::IngestResult Repository::ingest(const QString& text)
{
    Stmt& s = *m_ingest;
    s.reset();
    s.bindText(1, text).bindText(2, textHash(text)).bindText(3, now());
    s.step();
    const IngestResult result{s.colInt(0), s.colInt(1) != 0};
    s.run();
    return result;
}

Repository::IngestResult Repository::ingestImage(const ImageClip& image)
{
    Transaction tx(m_db);
    Stmt& s = *m_ingestImage;
    s.reset();
    const QString stamp = image.copiedAt.isValid() ? image.copiedAt.toUTC().toString(Qt::ISODateWithMs) : now();
    s.bindText(1, image.label()).bindText(2, QStringLiteral("image:") + image.pixelHash).bindText(3, stamp);
    s.step();
    const IngestResult result{s.colInt(0), s.colInt(1) != 0};
    s.run();
    if (result.inserted) {
        Stmt store(m_db, "INSERT INTO clip_images(clip_id, png, thumbnail, width, height) VALUES (?1, ?2, ?3, ?4, ?5)");
        store.bindInt(1, result.id).bindBlob(2, image.png).bindBlob(3, image.thumbnail);
        store.bindInt(4, image.width).bindInt(5, image.height);
        store.run();
    }
    tx.commit();
    return result;
}

Repository::IngestResult Repository::ingestSecret(const QByteArray& sealed, const QString& match,
                                                  const QString& note, const QString& group)
{
    Transaction tx(m_db);
    QString hash = match.isEmpty()
        ? QStringLiteral("secret:unmatched:") + QString::fromLatin1(PasswordVault::randomBytes(16).toHex())
        : match;
    if (!m_secretsPerNote && !match.isEmpty()) {
        if (const auto existing = findSecret(match)) {
            hash = existing->contentHash; // the same password, whatever its note
        }
    } else if (!match.isEmpty()) {
        // The same password under another note is another copy: it needs a hash of its own.
        const auto same = findSecrets(match);
        for (const Entry& e : same) {
            if (e.note == note.trimmed()) {
                hash = e.contentHash; // this one is copied again
                break;
            }
            hash = match + QLatin1Char(':') + QString::fromLatin1(PasswordVault::randomBytes(8).toHex());
        }
    }
    Stmt& s = *m_ingestSecret;
    s.reset();
    s.bindText(1, secretLabel()).bindText(2, hash).bindText(3, now());
    s.step();
    const IngestResult result{s.colInt(0), s.colInt(1) != 0};
    s.run();
    if (result.inserted) {
        Stmt store(m_db, "INSERT INTO clip_secrets(clip_id, sealed) VALUES (?1, ?2)");
        store.bindInt(1, result.id).bindBlob(2, sealed);
        store.run();
        if (!note.trimmed().isEmpty()) {
            setNote(result.id, note);
        }
        if (!group.trimmed().isEmpty()) {
            setGroup(result.id, group);
        }
    }
    tx.commit();
    return result;
}

QByteArray Repository::sealedSecret(qint64 id)
{
    Stmt s(m_db, "SELECT sealed FROM clip_secrets WHERE clip_id = ?1");
    s.bindInt(1, id);
    return s.step() ? s.colBlob(0) : QByteArray();
}

std::vector<std::pair<qint64, QByteArray>> Repository::allSealedSecrets()
{
    Stmt s(m_db, R"(SELECT c.id, s.sealed FROM clips c JOIN clip_secrets s ON s.clip_id = c.id
                    ORDER BY c.last_used DESC, c.id DESC)");
    std::vector<std::pair<qint64, QByteArray>> result;
    while (s.step()) {
        result.emplace_back(s.colInt(0), s.colBlob(1));
    }
    return result;
}

std::vector<Entry> Repository::findSecrets(const QString& match)
{
    if (match.isEmpty()) {
        return {};
    }
    // "secret:<hex>" for the first copy of a password, "secret:<hex>:<random>" for the same
    // password under another note (passwords_per_note).
    Stmt s(m_db, "SELECT " CLIP_COLUMNS " FROM clips c WHERE c.secret = 1 AND "
                 "(c.content_hash = ?1 OR substr(c.content_hash, 1, length(?1) + 1) = ?1 || ':') "
                 "ORDER BY c.last_used DESC, c.id DESC");
    s.bindText(1, match);
    return collect(s);
}

std::optional<Entry> Repository::findSecret(const QString& match)
{
    auto rows = findSecrets(match);
    return rows.empty() ? std::nullopt : std::optional<Entry>(std::move(rows.front()));
}

bool Repository::hasSecret(const QString& match, const QString& note)
{
    for (const Entry& e : findSecrets(match)) {
        if (!m_secretsPerNote || e.note == note.trimmed()) {
            return true;
        }
    }
    return false;
}

qint64 Repository::convertToSecret(qint64 id, const QByteArray& sealed, const QString& match)
{
    qint64 holder = id;
    {
        Transaction tx(m_db);
        const auto self = get(id);
        std::optional<Entry> existing;
        for (const Entry& e : findSecrets(match)) {
            if (e.id != id && (!m_secretsPerNote || (self && e.note == self->note))) {
                existing = e;
                break;
            }
        }
        if (existing) {
            remove(id); // the same password is already kept
            holder = existing->id;
        } else {
            Stmt update(m_db, "UPDATE clips SET content = ?2, content_hash = ?3, secret = 1, kind = 'text' WHERE id = ?1");
            // The same password under another note (passwords_per_note) needs a hash of its own.
            const bool taken = !match.isEmpty() && !findSecrets(match).empty();
            const QString hash = match.isEmpty() ? QStringLiteral("secret:unmatched:") + QString::fromLatin1(PasswordVault::randomBytes(16).toHex())
                : taken ? match + QLatin1Char(':') + QString::fromLatin1(PasswordVault::randomBytes(8).toHex())
                        : match;
            update.bindInt(1, id).bindText(2, secretLabel()).bindText(3, hash);
            update.run();
            clearRich(id); // the plain text is gone; so is its formatted copy
            Stmt store(m_db, "INSERT OR REPLACE INTO clip_secrets(clip_id, sealed) VALUES (?1, ?2)");
            store.bindInt(1, id).bindBlob(2, sealed);
            store.run();
        }
        tx.commit();
    }
    scrub();
    return holder;
}

qint64 Repository::convertToText(qint64 id, const QString& text)
{
    qint64 holder = id;
    Transaction tx(m_db);
    Stmt find(m_db, "SELECT id FROM clips WHERE content_hash = ?1");
    find.bindText(1, textHash(text));
    if (find.step() && find.colInt(0) != id) {
        holder = find.colInt(0);
        find.reset();
        remove(id); // the same text is already kept as a copy
    } else {
        find.reset();
        Stmt update(m_db, "UPDATE clips SET content = ?2, content_hash = ?3, secret = 0 WHERE id = ?1");
        update.bindInt(1, id).bindText(2, text).bindText(3, textHash(text));
        update.run();
        Stmt drop(m_db, "DELETE FROM clip_secrets WHERE clip_id = ?1");
        drop.bindInt(1, id);
        drop.run();
    }
    tx.commit();
    return holder;
}

qint64 Repository::countSecrets()
{
    Stmt s(m_db, "SELECT count(*) FROM clips WHERE secret = 1");
    s.step();
    return s.colInt(0);
}

std::optional<PasswordVault::Stored> Repository::loadVault()
{
    Stmt s(m_db, "SELECT salt, kdf_blocks, kdf_passes, public_key, wrapped_secret, hello_wrapped FROM vault WHERE id = 1");
    if (!s.step()) {
        return std::nullopt;
    }
    PasswordVault::Stored stored;
    stored.salt = s.colBlob(0);
    stored.kdfBlocks = quint32(s.colInt(1));
    stored.kdfPasses = quint32(s.colInt(2));
    stored.publicKey = s.colBlob(3);
    stored.wrappedSecret = s.colBlob(4);
    stored.helloWrapped = s.colBlob(5);
    return stored.isValid() ? std::optional(stored) : std::nullopt;
}

void Repository::saveVault(const PasswordVault::Stored& stored)
{
    Stmt s(m_db, R"(INSERT OR REPLACE INTO vault(id, salt, kdf_blocks, kdf_passes, public_key, wrapped_secret, hello_wrapped)
                    VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6))");
    s.bindBlob(1, stored.salt).bindInt(2, stored.kdfBlocks).bindInt(3, stored.kdfPasses);
    s.bindBlob(4, stored.publicKey).bindBlob(5, stored.wrappedSecret);
    if (stored.helloWrapped.isEmpty()) {
        s.bindNull(6);
    } else {
        s.bindBlob(6, stored.helloWrapped);
    }
    s.run();
}

void Repository::rekeyVault(const std::vector<std::pair<qint64, QByteArray>>& resealed,
                            const PasswordVault::Stored& stored)
{
    {
        Transaction tx(m_db);
        Stmt update(m_db, "UPDATE clip_secrets SET sealed = ?2 WHERE clip_id = ?1");
        for (const auto& [id, sealed] : resealed) {
            update.reset();
            update.bindInt(1, id).bindBlob(2, sealed);
            update.run();
        }
        saveVault(stored);
        tx.commit();
    }
    scrub();
}

void Repository::resetVault()
{
    {
        Transaction tx(m_db);
        m_db.exec("DELETE FROM clips WHERE secret = 1;");
        m_db.exec("DELETE FROM vault;");
        m_db.exec("DELETE FROM meta WHERE key = 'vault_open';");
        tx.commit();
    }
    scrub();
}

bool Repository::vaultOpen()
{
    Stmt s(m_db, "SELECT value FROM meta WHERE key = 'vault_open'");
    return s.step() && s.colText(0) == QLatin1String("1");
}

void Repository::setVaultOpen(bool open)
{
    Stmt s(m_db, "INSERT INTO meta(key, value) VALUES ('vault_open', ?1) "
                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bindText(1, open ? QStringLiteral("1") : QStringLiteral("0")).run();
}

void Repository::scrub()
{
    m_db.exec("INSERT INTO clips_fts(clips_fts) VALUES ('optimize');");
    if (!checkpoint()) {
        qWarning() << "the write-ahead log is still in use; it is folded in later";
    }
}

void Repository::setRich(qint64 id, const QString& html)
{
    Stmt s(m_db, "INSERT OR REPLACE INTO clip_rich(clip_id, html) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM clips WHERE id = ?1)");
    s.bindInt(1, id).bindText(2, html);
    s.run();
}

void Repository::clearRich(qint64 id)
{
    Stmt s(m_db, "DELETE FROM clip_rich WHERE clip_id = ?1");
    s.bindInt(1, id);
    s.run();
}

void Repository::clearAllRich()
{
    {
        Stmt any(m_db, "SELECT 1 FROM clip_rich LIMIT 1");
        if (!any.step()) {
            return; // nothing kept: no write, no checkpoint
        }
    }
    Transaction tx(m_db);
    m_db.exec("DELETE FROM clip_rich");
    tx.commit();
    checkpoint(); // freed pages were zeroed (secure_delete); the log may still hold the text
}

QString Repository::richText(qint64 id)
{
    Stmt s(m_db, "SELECT html FROM clip_rich WHERE clip_id = ?1");
    s.bindInt(1, id);
    return s.step() ? s.colText(0) : QString();
}

QByteArray Repository::imageData(qint64 id)
{
    Stmt s(m_db, "SELECT png FROM clip_images WHERE clip_id = ?1");
    s.bindInt(1, id);
    return s.step() ? s.colBlob(0) : QByteArray();
}

qint64 Repository::nextPictureToIndex()
{
    Stmt s(m_db, "SELECT i.clip_id FROM clip_images i LEFT JOIN clip_text t ON t.clip_id = i.clip_id "
                 "WHERE t.clip_id IS NULL ORDER BY i.clip_id DESC LIMIT 1");
    return s.step() ? s.colInt(0) : 0;
}

void Repository::setPictureText(qint64 id, const QString& text)
{
    Stmt s(m_db, "INSERT OR REPLACE INTO clip_text(clip_id, text) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM clips WHERE id = ?1)");
    s.bindInt(1, id).bindText(2, text);
    s.run();
}

void Repository::clearPictureText()
{
    Transaction tx(m_db);
    m_db.exec("DELETE FROM clip_text");
    tx.commit();
    checkpoint(); // freed pages were zeroed (secure_delete); the log may still hold the text
}

qint64 Repository::countIndexedPictures()
{
    Stmt s(m_db, "SELECT count(*) FROM clip_text WHERE text <> ''");
    return s.step() ? s.colInt(0) : 0;
}

void Repository::loadImageDetails(Entry& entry)
{
    Stmt& s = *m_imageDetails;
    s.reset();
    s.bindInt(1, entry.id);
    if (s.step()) {
        entry.imageWidth = int(s.colInt(0));
        entry.imageHeight = int(s.colInt(1));
        entry.thumbnail = s.colBlob(2);
    }
    s.reset();
}

std::vector<Entry> Repository::collect(Stmt& stmt)
{
    std::vector<Entry> result;
    while (stmt.step()) {
        result.push_back(readEntry(stmt));
    }
    stmt.reset();
    // Lists show a handful of images at most: their thumbnails are fetched one by one,
    // keeping every list query free of joins.
    for (Entry& e : result) {
        if (e.image) {
            loadImageDetails(e);
        }
    }
    return result;
}

std::vector<Entry> Repository::recent(int limit, bool unsavedOnly)
{
    Stmt& s = unsavedOnly ? *m_recentUnsaved : *m_recentAll;
    s.reset();
    s.bindInt(1, limit);
    return collect(s);
}

std::vector<Entry> Repository::recentUnpinned(int limit)
{
    m_recentUnpinned->reset();
    m_recentUnpinned->bindInt(1, limit);
    return collect(*m_recentUnpinned);
}

void Repository::takeOffRecent(qint64 id)
{
    Stmt s(m_db, "UPDATE clips SET off_recent = 1 WHERE id = ?1 AND is_saved = 1");
    s.bindInt(1, id).run();
}

qint64 Repository::countRecent()
{
    Stmt s(m_db, "SELECT count(*) FROM clips c WHERE c.pin_position IS NULL AND " IN_RECENT);
    s.step();
    return s.colInt(0);
}

qint64 Repository::clearRecent()
{
    qint64 cleared = 0;
    {
        Transaction tx(m_db);
        // Filed copies only leave Recent, unfiled ones are deleted: count what Recent showed.
        Stmt count(m_db, "SELECT count(*) FROM clips c WHERE c.pin_position IS NULL AND " IN_RECENT);
        count.step();
        cleared = count.colInt(0);
        Stmt off(m_db, "UPDATE clips SET off_recent = 1 WHERE is_saved = 1 AND pin_position IS NULL");
        off.run();
        setMeta(m_db, "sync_share_deletes", QStringLiteral("1"));
        Stmt del(m_db, "DELETE FROM clips WHERE is_saved = 0");
        del.run();
        setMeta(m_db, "sync_share_deletes", QStringLiteral("0"));
        m_db.exec("INSERT INTO clips_fts(clips_fts) VALUES ('optimize');");
        tx.commit();
    }
    if (!checkpoint()) {
        qWarning() << "Recent cleared, but the write-ahead log is still in use; it is folded in later";
    }
    return cleared;
}

void Repository::setGroupHiddenFromRecent(const QString& path, bool hidden)
{
    const QString normalized = groups::normalize(path);
    if (normalized.isEmpty()) {
        return;
    }
    if (hidden) {
        createGroup(normalized);
    }
    Stmt s(m_db, "UPDATE groups SET hide_from_recent = ?2 WHERE path = ?1");
    s.bindText(1, normalized).bindInt(2, hidden ? 1 : 0).run();
}

bool Repository::groupHiddenFromRecent(const QString& path)
{
    Stmt s(m_db, "SELECT hide_from_recent FROM groups WHERE path = ?1");
    s.bindText(1, groups::normalize(path));
    return s.step() && s.colInt(0) == 1;
}

void Repository::setFavoritesHiddenFromRecent(bool hidden)
{
    Stmt s(m_db, "INSERT INTO meta(key, value) VALUES ('favorites_hide_from_recent', ?1) "
                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bindText(1, hidden ? QStringLiteral("1") : QStringLiteral("0")).run();
}

bool Repository::favoritesHiddenFromRecent()
{
    Stmt s(m_db, "SELECT value FROM meta WHERE key = 'favorites_hide_from_recent'");
    return s.step() && s.colText(0) == QLatin1String("1");
}

std::vector<Entry> Repository::pinned(int limit)
{
    m_pinned->reset();
    m_pinned->bindInt(1, limit);
    return collect(*m_pinned);
}

std::vector<Entry> Repository::favorites()
{
    Stmt s(m_db, "SELECT " CLIP_COLUMNS R"( FROM clips c
        WHERE c.is_saved = 1 AND c.is_favorite = 1 AND (c.group_name IS NULL OR c.group_name = '')
        ORDER BY c.last_used DESC, c.id DESC)"); // is_saved = 1 (implied by the star) lets the index skip unfiled copies
    return collect(s);
}

std::vector<Entry> Repository::search(const QString& query, int limit, SearchMode mode)
{
    if (query.contains(QLatin1Char(':'))) {
        const SearchFilter filter = SearchFilter::parse(query);
        if (filter.hasFilters()) {
            return searchFiltered(filter, limit);
        }
    }
    const QStringList tokens = fts::tokenize(query);
    if (tokens.isEmpty()) {
        return {};
    }
    // Pictures are also found by the text read from them.
    if (mode == SearchMode::ForceLike || !fts::canUseTrigram(tokens)) {
        return mergeNewest(searchLike(tokens, limit), searchPictureText(tokens, limit), limit);
    }
    Stmt& s = *m_searchFts;
    s.reset();
    s.bindText(1, fts::matchExpression(tokens)).bindInt(2, limit);
    return mergeNewest(collect(s), searchPictureText(tokens, limit), limit);
}

std::vector<Entry> Repository::searchPictureText(const QStringList& tokens, int limit)
{
    QStringList where;
    for (int i = 1; i <= tokens.size(); ++i) {
        where << QStringLiteral(R"(t.text LIKE ?%1 ESCAPE '\')").arg(i);
    }
    const QString sql = QStringLiteral("SELECT " CLIP_COLUMNS " FROM clips c JOIN clip_text t ON t.clip_id = c.id "
                                       "WHERE %1 ORDER BY c.last_used DESC, c.id DESC LIMIT ?%2")
                            .arg(where.join(QLatin1String(" AND ")))
                            .arg(tokens.size() + 1);
    Stmt s(m_db, sql.toUtf8());
    for (int i = 0; i < tokens.size(); ++i) {
        s.bindText(i + 1, fts::likePattern(tokens[i]));
    }
    s.bindInt(int(tokens.size()) + 1, limit);
    return collect(s);
}

// Both lists are newest first; the result is too, without a copy twice, at most `limit` long.
std::vector<Entry> Repository::mergeNewest(std::vector<Entry> a, std::vector<Entry> b, int limit)
{
    if (b.empty()) {
        return a;
    }
    for (Entry& e : b) {
        const bool known = std::any_of(a.begin(), a.end(), [&e](const Entry& x) { return x.id == e.id; });
        if (!known) {
            a.push_back(std::move(e));
        }
    }
    std::stable_sort(a.begin(), a.end(), [](const Entry& x, const Entry& y) {
        return x.lastUsed != y.lastUsed ? x.lastUsed > y.lastUsed : x.id > y.id;
    });
    if (int(a.size()) > limit) {
        a.resize(size_t(limit));
    }
    return a;
}

Stmt& Repository::likeStatement(int tokenCount)
{
    auto it = m_likeByTokenCount.find(tokenCount);
    if (it != m_likeByTokenCount.end()) {
        return *it->second;
    }

    // Every token must match one of the three columns; note/group hits are counted for ranking.
    QStringList where;
    QStringList noteHits;
    QStringList groupHits;
    for (int i = 1; i <= tokenCount; ++i) {
        const QString p = QStringLiteral("?%1").arg(i);
        const QString note = QStringLiteral(R"(c.tag_or_note LIKE %1 ESCAPE '\')").arg(p);
        const QString group = QStringLiteral(R"(c.group_name LIKE %1 ESCAPE '\')").arg(p);
        const QString content = QStringLiteral(R"(c.content LIKE %1 ESCAPE '\')").arg(p);
        where << QStringLiteral("(%1 OR %2 OR %3)").arg(note, group, content);
        noteHits << QStringLiteral("coalesce(%1, 0)").arg(note);
        groupHits << QStringLiteral("coalesce(%1, 0)").arg(group);
    }
    const QString sql = QStringLiteral("SELECT " CLIP_COLUMNS ", (%1) AS note_hits, (%2) AS group_hits "
                                       "FROM clips c WHERE %3 "
                                       "ORDER BY c.last_used DESC, c.id DESC LIMIT ?%4")
                            .arg(noteHits.join(QLatin1String(" + ")), groupHits.join(QLatin1String(" + ")),
                                 where.join(QLatin1String(" AND ")))
                            .arg(tokenCount + 1);

    auto stmt = std::make_unique<Stmt>(m_db, sql.toUtf8());
    Stmt& ref = *stmt;
    m_likeByTokenCount.emplace(tokenCount, std::move(stmt));
    return ref;
}

std::vector<Entry> Repository::searchFiltered(const SearchFilter& filter, int limit)
{
    QStringList where;
    QStringList binds; // text values, bound in order as ?1, ?2, ...
    const auto bind = [&binds](const QString& value) {
        binds << value;
        return QStringLiteral("?%1").arg(binds.size());
    };
    if (filter.images) {
        where << QStringLiteral("c.kind = 'image'");
    }
    if (filter.textOnly) {
        where << QStringLiteral("c.kind = 'text' AND c.secret = 0");
    }
    if (filter.pinned) {
        where << QStringLiteral("c.pin_position IS NOT NULL");
    }
    if (filter.starred) {
        where << QStringLiteral("c.is_favorite = 1");
    }
    if (filter.group) {
        where << (filter.group->isEmpty()
                      ? QStringLiteral("c.group_name <> ''")
                      : QStringLiteral(R"(c.group_name LIKE %1 ESCAPE '\')").arg(bind(fts::likePattern(*filter.group))));
    }
    if (filter.note) {
        where << (filter.note->isEmpty()
                      ? QStringLiteral("c.tag_or_note <> ''")
                      : QStringLiteral(R"(c.tag_or_note LIKE %1 ESCAPE '\')").arg(bind(fts::likePattern(*filter.note))));
    }
    if (filter.since) {
        where << QStringLiteral("c.last_used >= %1").arg(bind(filter.since->toString(Qt::ISODateWithMs)));
    }
    for (const QString& word : filter.words) {
        const QString p = bind(fts::likePattern(word));
        where << QStringLiteral(R"((c.tag_or_note LIKE %1 ESCAPE '\' OR c.group_name LIKE %1 ESCAPE '\' OR c.content LIKE %1 ESCAPE '\'
            OR EXISTS (SELECT 1 FROM clip_text t WHERE t.clip_id = c.id AND t.text LIKE %1 ESCAPE '\')))").arg(p);
    }
    const QString sql = QStringLiteral("SELECT " CLIP_COLUMNS " FROM clips c WHERE %1 "
                                       "ORDER BY c.last_used DESC, c.id DESC LIMIT ?%2")
                            .arg(where.join(QLatin1String(" AND ")))
                            .arg(binds.size() + 1);
    Stmt s(m_db, sql.toUtf8());
    for (int i = 0; i < binds.size(); ++i) {
        s.bindText(i + 1, binds[i]);
    }
    s.bindInt(int(binds.size()) + 1, limit);
    return collect(s);
}

std::vector<Entry> Repository::searchLike(const QStringList& tokens, int limit)
{
    Stmt& s = likeStatement(int(tokens.size()));
    s.reset();
    for (int i = 0; i < tokens.size(); ++i) {
        s.bindText(i + 1, fts::likePattern(tokens[i]));
    }
    s.bindInt(int(tokens.size()) + 1, limit);
    return collect(s);
}

std::optional<Entry> Repository::get(qint64 id)
{
    m_get->reset();
    m_get->bindInt(1, id);
    auto rows = collect(*m_get);
    if (rows.empty()) {
        return std::nullopt;
    }
    return std::move(rows.front());
}

void Repository::touch(qint64 id)
{
    m_touch->reset();
    m_touch->bindInt(1, id).bindText(2, now());
    m_touch->run();
}

int Repository::starsIn(const QString& group)
{
    Stmt s(m_db, group.isEmpty()
                     ? "SELECT count(*) FROM clips WHERE is_favorite = 1 AND (group_name IS NULL OR group_name = '')"
                     : "SELECT count(*) FROM clips WHERE is_favorite = 1 AND group_name = ?1");
    if (!group.isEmpty()) {
        s.bindText(1, group);
    }
    s.step();
    return int(s.colInt(0));
}

Repository::Change Repository::roomForStar(const QString& group)
{
    if (group.isEmpty()) {
        return starsIn(group) < m_limits.favorites ? Change::Done : Change::FavoritesFull;
    }
    return starsIn(group) < m_limits.starsPerGroup ? Change::Done : Change::GroupStarsFull;
}

Repository::Change Repository::setFavorite(qint64 id, bool favorite)
{
    const auto e = get(id);
    if (!e || e->favorite == favorite) {
        return Change::Done;
    }
    if (favorite) {
        if (const Change room = roomForStar(e->group); room != Change::Done) {
            return room;
        }
    }
    m_setFavorite->reset();
    m_setFavorite->bindInt(1, id).bindInt(2, favorite ? 1 : 0);
    m_setFavorite->run();
    return Change::Done;
}

Repository::Change Repository::setGroup(qint64 id, const QString& group)
{
    const QString target = groups::normalize(group);
    const auto e = get(id);
    if (!e) {
        return Change::Done;
    }
    if (e->favorite && e->group != target) {
        // The star moves with the clip: into the target group, or into Favorites.
        if (const Change room = roomForStar(target); room != Change::Done) {
            return room;
        }
    }
    // A copy that looks like a key or a password (kept as plain text when passwords are not kept) teaches nothing.
    const bool teach = m_autoGrouping && e->group != target && !e->image && !e->secret && !looksSecret(e->content);
    if (teach) {
        ensureAutoSeeded(); // before the move, so the copy is not counted twice
    }
    if (!target.isEmpty()) {
        createGroup(target);
    }
    m_setGroup->reset();
    m_setGroup->bindInt(1, id).bindTextOrNull(2, target);
    m_setGroup->run();
    if (e->autoFiled && e->group != target) {
        Stmt flag(m_db, "UPDATE clips SET auto_filed = 0 WHERE id = ?1");
        flag.bindInt(1, id).run();
    }
    if (teach) {
        // A copy that was filed automatically and is moved or unfiled was a wrong guess: that counts more.
        const bool correction = e->autoFiled;
        if (correction && !e->group.isEmpty()) {
            learn(e->group, e->content, -autogroup::kCorrectionWeight);
        }
        if (!target.isEmpty()) {
            learn(target, e->content, correction ? autogroup::kCorrectionWeight : 1);
        }
    }
    return Change::Done;
}

Repository::Change Repository::setPinned(qint64 id, bool pin)
{
    const auto e = get(id);
    if (!e || e->pinned() == pin) {
        return Change::Done;
    }
    if (!pin) {
        Stmt s(m_db, "UPDATE clips SET pin_position = NULL WHERE id = ?1");
        s.bindInt(1, id).run();
        return Change::Done;
    }
    Stmt count(m_db, "SELECT count(*), coalesce(max(pin_position) + 1, 0) FROM clips WHERE pin_position IS NOT NULL");
    count.step();
    if (count.colInt(0) >= m_limits.pins) {
        return Change::PinsFull;
    }
    const qint64 position = count.colInt(1);
    count.reset();
    Stmt s(m_db, "UPDATE clips SET pin_position = ?2 WHERE id = ?1");
    s.bindInt(1, id).bindInt(2, position).run();
    return Change::Done;
}

bool Repository::movePinned(qint64 id, int delta)
{
    std::vector<qint64> order;
    for (const Entry& e : pinned(m_limits.pins + 1000)) { // everything pinned, even over a lowered cap
        order.push_back(e.id);
    }
    const auto it = std::find(order.begin(), order.end(), id);
    if (it == order.end()) {
        return false;
    }
    const auto from = std::size_t(it - order.begin());
    const qint64 to = qint64(from) + delta;
    if (delta == 0 || to < 0 || to >= qint64(order.size())) {
        return false;
    }
    std::swap(order[from], order[std::size_t(to)]);
    Transaction tx(m_db);
    Stmt s(m_db, "UPDATE clips SET pin_position = ?2 WHERE id = ?1");
    for (std::size_t i = 0; i < order.size(); ++i) {
        s.reset();
        s.bindInt(1, order[i]).bindInt(2, qint64(i));
        s.run();
    }
    tx.commit();
    return true;
}

namespace {

// LIKE pattern matching everything strictly below `path`.
QString descendantsPattern(const QString& path)
{
    QString escaped;
    for (const QChar c : path) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('%') || c == QLatin1Char('_')) {
            escaped += QLatin1Char('\\');
        }
        escaped += c;
    }
    return escaped + QStringLiteral("/%");
}

} // namespace

std::vector<Repository::GroupInfo> Repository::groupTree()
{
    struct Row {
        GroupInfo info;
        qint64 position; // -1 = unordered
    };
    Stmt s(m_db, R"(
        SELECT g.path, (SELECT count(*) FROM clips c WHERE c.group_name = g.path), g.position
        FROM groups g)");
    QHash<QString, std::vector<Row>> children; // parent path ("" = top level) -> its groups
    while (s.step()) {
        const QString path = s.colText(0);
        children[groups::parent(path)].push_back({{path, int(s.colInt(1))}, s.colIsNull(2) ? -1 : s.colInt(2)});
    }
    for (auto& siblings : children) {
        // Ordered groups first, in their order; the rest after them, by name.
        std::sort(siblings.begin(), siblings.end(), [](const Row& a, const Row& b) {
            if ((a.position < 0) != (b.position < 0)) {
                return a.position >= 0;
            }
            if (a.position != b.position) {
                return a.position < b.position;
            }
            return QString::compare(groups::leaf(a.info.path), groups::leaf(b.info.path), Qt::CaseInsensitive) < 0;
        });
    }
    // Depth first, so every parent comes before its children.
    std::vector<GroupInfo> result;
    const std::function<void(const QString&)> visit = [&](const QString& parent) {
        const auto it = children.constFind(parent);
        if (it == children.constEnd()) {
            return;
        }
        for (const Row& row : *it) {
            result.push_back(row.info);
            visit(row.info.path);
        }
    };
    visit(QString());
    return result;
}

std::vector<Entry> Repository::groupedEntries()
{
    Stmt s(m_db, "SELECT " CLIP_COLUMNS R"( FROM clips c
        WHERE c.group_name IS NOT NULL AND c.group_name <> ''
        ORDER BY c.is_favorite DESC, c.last_used DESC, c.id DESC)");
    return collect(s);
}

QString Repository::createGroup(const QString& path)
{
    const QString normalized = groups::normalize(path);
    if (normalized.isEmpty()) {
        return {};
    }
    Stmt insert(m_db, "INSERT OR IGNORE INTO groups(path, created_at) VALUES (?1, ?2)");
    for (QString level = normalized; !level.isEmpty(); level = groups::parent(level)) {
        insert.reset();
        insert.bindText(1, level).bindText(2, now());
        insert.run();
    }
    return normalized;
}

bool Repository::renameGroup(const QString& from, const QString& to, QString* error)
{
    const auto fail = [&](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    const QString source = groups::normalize(from);
    const QString target = groups::normalize(to);
    if (source.isEmpty() || target.isEmpty()) {
        return fail(QStringLiteral("a group needs a name"));
    }
    if (source == target) {
        return true;
    }
    if (groups::isWithin(target, source)) {
        return fail(QStringLiteral("cannot move a group into itself"));
    }
    if (groups().contains(target)) {
        return fail(QStringLiteral("a group named %1 already exists").arg(groups::display(target)));
    }

    Transaction tx(m_db);
    if (!groups::parent(target).isEmpty()) {
        createGroup(groups::parent(target));
    }
    // ?1 = old prefix, ?2 = new prefix, ?3 = LIKE pattern for everything below the old one.
    for (const char* sql : {
             "UPDATE groups SET path = ?2 || substr(path, length(?1) + 1) "
             "WHERE path = ?1 OR path LIKE ?3 ESCAPE '\\'",
             "UPDATE clips SET group_name = ?2 || substr(group_name, length(?1) + 1) "
             "WHERE group_name = ?1 OR group_name LIKE ?3 ESCAPE '\\'",
             "UPDATE auto_examples SET group_name = ?2 || substr(group_name, length(?1) + 1) "
             "WHERE group_name = ?1 OR group_name LIKE ?3 ESCAPE '\\'",
             "UPDATE auto_tokens SET group_name = ?2 || substr(group_name, length(?1) + 1) "
             "WHERE group_name = ?1 OR group_name LIKE ?3 ESCAPE '\\'",
         }) {
        Stmt s(m_db, sql);
        s.bindText(1, source).bindText(2, target).bindText(3, descendantsPattern(source));
        s.run();
    }
    if (groups::parent(target) != groups::parent(source)) {
        // Its place among the old siblings means nothing among the new ones: go last.
        Stmt s(m_db, "UPDATE groups SET position = NULL WHERE path = ?1");
        s.bindText(1, target).run();
    }
    tx.commit();
    m_autoModel.reset();
    return true;
}

void Repository::deleteGroup(const QString& path)
{
    const QString normalized = groups::normalize(path);
    if (normalized.isEmpty()) {
        return;
    }
    Transaction tx(m_db);
    for (const char* sql : {
             "UPDATE clips SET group_name = NULL, auto_filed = 0 WHERE group_name = ?1 OR group_name LIKE ?2 ESCAPE '\\'",
             "DELETE FROM auto_examples WHERE group_name = ?1 OR group_name LIKE ?2 ESCAPE '\\'",
             "DELETE FROM auto_tokens WHERE group_name = ?1 OR group_name LIKE ?2 ESCAPE '\\'",
             "DELETE FROM groups WHERE path = ?1 OR path LIKE ?2 ESCAPE '\\'",
         }) {
        Stmt s(m_db, sql);
        s.bindText(1, normalized).bindText(2, descendantsPattern(normalized));
        s.run();
    }
    tx.commit();
    m_autoModel.reset();
    ++m_autoForgets;
    checkpoint(); // the log may still hold the deleted rows
}

void Repository::setNote(qint64 id, const QString& note)
{
    m_setNote->reset();
    m_setNote->bindInt(1, id).bindTextOrNull(2, note.trimmed());
    m_setNote->run();
}

Repository::ContentChange Repository::setContent(qint64 id, const QString& text, qint64 maxBytes)
{
    if (text.trimmed().isEmpty()) {
        return ContentChange::Empty;
    }
    if (text.size() * 3 > maxBytes && text.toUtf8().size() > maxBytes) {
        return ContentChange::TooLarge;
    }
    Transaction tx(m_db);
    {
        Stmt kind(m_db, "SELECT kind, secret FROM clips WHERE id = ?1");
        kind.bindInt(1, id);
        if (!kind.step()) {
            return ContentChange::Missing;
        }
        if (kind.colText(0) != QLatin1String("text") || kind.colInt(1) != 0) {
            return ContentChange::NotText;
        }
    }
    const QString hash = textHash(text);
    {
        Stmt find(m_db, "SELECT id FROM clips WHERE content_hash = ?1 AND id != ?2");
        find.bindText(1, hash).bindInt(2, id);
        if (find.step()) {
            return ContentChange::Duplicate;
        }
    }
    Stmt update(m_db, "UPDATE clips SET content = ?2, content_hash = ?3 WHERE id = ?1");
    update.bindInt(1, id).bindText(2, text).bindText(3, hash);
    update.run();
    clearRich(id); // the formatted copy belongs to the old text
    tx.commit();
    return ContentChange::Done;
}

std::optional<qint64> Repository::newestId()
{
    Stmt s(m_db, "SELECT id FROM clips ORDER BY last_used DESC, id DESC LIMIT 1");
    if (!s.step()) {
        return std::nullopt;
    }
    return s.colInt(0);
}

void Repository::remove(qint64 id)
{
    // A deletion the user asked for: with sync on it is written to the sync file (sync_share_deletes).
    // A copy that looks like a secret leaves no hash behind (the hash of a short secret can be guessed back).
    const auto doomed = get(id);
    const bool secretLooking = doomed && !doomed->secret && !doomed->image && looksSecret(doomed->content);
    setMeta(m_db, "sync_share_deletes", QStringLiteral("1"));
    try {
        m_remove->reset();
        m_remove->bindInt(1, id);
        m_remove->run();
    } catch (...) {
        setMeta(m_db, "sync_share_deletes", QStringLiteral("0"));
        throw;
    }
    setMeta(m_db, "sync_share_deletes", QStringLiteral("0"));
    if (secretLooking) {
        Stmt forgetHash(m_db, "DELETE FROM sync_tombstones WHERE content_hash = ?1");
        forgetHash.bindText(1, doomed->contentHash).run();
    }
}

void Repository::setPasteOnce(qint64 id, bool once)
{
    Stmt s(m_db, "UPDATE clips SET paste_once = ?2 WHERE id = ?1");
    s.bindInt(1, id).bindInt(2, once ? 1 : 0).run();
}

std::optional<Repository::Removed> Repository::snapshot(qint64 id)
{
    const std::optional<Entry> entry = get(id);
    if (!entry) {
        return std::nullopt;
    }
    Removed r;
    r.entry = *entry;
    {
        Stmt s(m_db, "SELECT off_recent FROM clips WHERE id = ?1");
        s.bindInt(1, id);
        r.offRecent = s.step() && s.colInt(0) != 0;
    }
    if (entry->image) {
        Stmt s(m_db, "SELECT png, thumbnail FROM clip_images WHERE clip_id = ?1");
        s.bindInt(1, id);
        if (s.step()) {
            r.png = s.colBlob(0);
            r.thumbnail = s.colBlob(1);
        }
    }
    if (entry->secret) {
        r.sealed = sealedSecret(id);
    }
    r.html = richText(id);
    return r;
}

bool Repository::forgetNow(qint64 id)
{
    bool deleted = false;
    {
        Transaction tx(m_db);
        Stmt s(m_db, "DELETE FROM clips WHERE id = ?1");
        s.bindInt(1, id).run();
        deleted = m_db.changes() > 0;
        m_db.exec("INSERT INTO clips_fts(clips_fts) VALUES ('optimize');");
        tx.commit();
    }
    if (deleted) {
        ++m_autoForgets; // what auto grouping cached about the copies is stale
        m_autoModel.reset();
    }
    if (!checkpoint()) {
        qWarning() << "copy forgotten, but the write-ahead log is still in use; it is folded in later";
    }
    return deleted;
}

qint64 Repository::restore(const Removed& removed)
{
    const Entry& e = removed.entry;
    if (const auto same = findByHash(e.contentHash)) {
        return *same;
    }
    Transaction tx(m_db);
    Stmt ins(m_db, "INSERT INTO clips(content, content_hash, created_at, last_used, kind, secret) "
                   "VALUES (?1, ?2, ?3, ?3, ?4, ?5) RETURNING id");
    ins.bindText(1, e.content).bindText(2, e.contentHash).bindText(3, now());
    ins.bindText(4, e.image ? QStringLiteral("image") : QStringLiteral("text")).bindInt(5, e.secret ? 1 : 0);
    ins.step();
    const qint64 id = ins.colInt(0);
    ins.reset();
    if (e.image) {
        Stmt store(m_db, "INSERT INTO clip_images(clip_id, png, thumbnail, width, height) VALUES (?1, ?2, ?3, ?4, ?5)");
        store.bindInt(1, id).bindBlob(2, removed.png).bindBlob(3, removed.thumbnail);
        store.bindInt(4, e.imageWidth).bindInt(5, e.imageHeight);
        store.run();
    }
    if (e.secret) {
        Stmt store(m_db, "INSERT INTO clip_secrets(clip_id, sealed) VALUES (?1, ?2)");
        store.bindInt(1, id).bindBlob(2, removed.sealed);
        store.run();
    }
    if (!removed.html.isEmpty()) {
        setRich(id, removed.html);
    }
    tx.commit();
    restoreFiling(id, e.createdAt, e.lastUsed, e.note, e.group, e.favorite, removed.offRecent); // its own transaction
    if (e.autoFiled) {
        Stmt flag(m_db, "UPDATE clips SET auto_filed = 1 WHERE id = ?1");
        flag.bindInt(1, id).run();
    }
    return id;
}

std::vector<Entry> Repository::allEntries()
{
    Stmt s(m_db, "SELECT " CLIP_COLUMNS " FROM clips c ORDER BY c.id");
    return collect(s);
}

std::optional<qint64> Repository::findByHash(const QString& contentHash)
{
    Stmt s(m_db, "SELECT id FROM clips WHERE content_hash = ?1");
    s.bindText(1, contentHash);
    if (!s.step()) {
        return std::nullopt;
    }
    return s.colInt(0);
}

void Repository::restoreFiling(qint64 id, const QDateTime& created, const QDateTime& lastUsed, const QString& note,
                               const QString& group, bool favorite, bool offRecent)
{
    Transaction tx(m_db);
    const QString path = groups::normalize(group);
    if (!path.isEmpty()) {
        createGroup(path);
    }
    // The columns the search index covers (note, group) are written only when there is something to write: an UPDATE
    // that names them rewrites the row's entries in the index (trigger clips_au) even when the value is the same, and
    // doing that for every copy of a restored backup made importing quadratic (3000 copies: 9 s, 4000: 12 s).
    const QString noteText = note.trimmed();
    const bool filed = !noteText.isEmpty() || !path.isEmpty();
    Stmt s(m_db, filed ? "UPDATE clips SET created_at = ?2, last_used = ?3, tag_or_note = ?4, group_name = ?5, "
                         "is_favorite = ?6, off_recent = ?7 WHERE id = ?1"
                       : "UPDATE clips SET created_at = ?2, last_used = ?3, is_favorite = ?6, off_recent = ?7 WHERE id = ?1");
    s.bindInt(1, id);
    s.bindText(2, (created.isValid() ? created : lastUsed).toUTC().toString(Qt::ISODateWithMs));
    s.bindText(3, (lastUsed.isValid() ? lastUsed : created).toUTC().toString(Qt::ISODateWithMs));
    if (filed) {
        s.bindTextOrNull(4, noteText);
        s.bindTextOrNull(5, path);
    }
    s.bindInt(6, favorite ? 1 : 0);
    s.bindInt(7, offRecent ? 1 : 0);
    s.run();
    tx.commit();
}

QSet<qint64> Repository::offRecentIds()
{
    QSet<qint64> ids;
    Stmt s(m_db, "SELECT id FROM clips WHERE off_recent = 1");
    while (s.step()) {
        ids.insert(s.colInt(0));
    }
    return ids;
}

void Repository::placeGroupLast(const QString& path)
{
    const QString normalized = groups::normalize(path);
    const QString parent = groups::parent(normalized);
    qint64 next = 0;
    Stmt all(m_db, "SELECT path, position FROM groups");
    while (all.step()) {
        if (all.colText(0) != normalized && groups::parent(all.colText(0)) == parent && !all.colIsNull(1)) {
            next = std::max(next, all.colInt(1) + 1);
        }
    }
    Stmt s(m_db, "UPDATE groups SET position = ?2 WHERE path = ?1");
    s.bindText(1, normalized).bindInt(2, next).run();
}

qint64 Repository::clearHistory(bool everything)
{
    qint64 deleted = 0;
    {
        Transaction tx(m_db);
        setMeta(m_db, "sync_share_deletes", QStringLiteral("1"));
        Stmt s(m_db, everything ? "DELETE FROM clips" : "DELETE FROM clips WHERE is_saved = 0");
        s.run();
        deleted = m_db.changes();
        setMeta(m_db, "sync_share_deletes", QStringLiteral("0"));
        if (everything) {
            // What was learned from the copies goes with them.
            m_db.exec("DELETE FROM auto_tokens");
            m_db.exec("DELETE FROM auto_examples");
            m_autoModel.reset();
            ++m_autoForgets;
        }
        // The search index only marks deleted rows; their text stays in its segments until
        // they are merged. Merge now, so nothing of the cleared copies is left in it.
        m_db.exec(everything ? "INSERT INTO clips_fts(clips_fts) VALUES ('delete-all');"
                             : "INSERT INTO clips_fts(clips_fts) VALUES ('optimize');");
        tx.commit();
    }
    // Freed pages were zeroed (secure_delete); the WAL still holds earlier copies of them
    // until a checkpoint folds it in and truncates it.
    if (!checkpoint()) {
        qWarning() << "history cleared, but the write-ahead log is still in use; it is folded in later";
    }
    return deleted;
}

bool Repository::checkpoint()
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        Stmt s(m_db, "PRAGMA wal_checkpoint(TRUNCATE)");
        // (busy, log frames, checkpointed frames): done when not busy and all were copied.
        if (s.step() && s.colInt(0) == 0 && s.colInt(1) == s.colInt(2)) {
            return true;
        }
    }
    return false;
}

qint64 Repository::prune(int maxUnsaved)
{
    m_prune->reset();
    m_prune->bindInt(1, maxUnsaved);
    m_prune->run();
    return m_db.changes();
}

std::vector<Repository::ForgetCandidate> Repository::forgetCandidates(const QDateTime& before, int limit)
{
    std::vector<ForgetCandidate> out;
    Stmt s(m_db, "SELECT id, content_hash, last_used FROM clips WHERE is_saved = 0 AND secret = 0 AND kind = 'text' "
                 "AND last_used < ?1 ORDER BY last_used ASC LIMIT ?2");
    s.bindText(1, before.toUTC().toString(Qt::ISODateWithMs)).bindInt(2, limit);
    while (s.step()) {
        out.push_back({s.colInt(0), s.colText(1), QDateTime::fromString(s.colText(2), Qt::ISODateWithMs)});
    }
    return out;
}

QString Repository::forgetText(qint64 id)
{
    Stmt s(m_db, "SELECT content FROM clips WHERE id = ?1 AND is_saved = 0 AND secret = 0 AND kind = 'text'");
    s.bindInt(1, id);
    return s.step() ? s.colText(0) : QString();
}

qint64 Repository::forget(const std::vector<std::pair<QDateTime, ForgetKind>>& cuts, const std::vector<qint64>& ids,
                          int limit, bool* more)
{
    qint64 deleted = 0;
    bool limited = false;
    {
        Transaction tx(m_db);
        for (const auto& [cutoff, kind] : cuts) {
            if (deleted >= limit) {
                limited = true;
                break;
            }
            const char* sql = kind == ForgetKind::Text
                ? "DELETE FROM clips WHERE id IN (SELECT id FROM clips WHERE is_saved = 0 AND secret = 0 AND kind = 'text' "
                  "AND last_used < ?1 ORDER BY last_used LIMIT ?2)"
                : kind == ForgetKind::Picture
                    ? "DELETE FROM clips WHERE id IN (SELECT id FROM clips WHERE is_saved = 0 AND secret = 0 "
                      "AND kind = 'image' AND last_used < ?1 ORDER BY last_used LIMIT ?2)"
                    : "DELETE FROM clips WHERE id IN (SELECT id FROM clips WHERE is_saved = 0 AND secret = 0 "
                      "AND last_used < ?1 ORDER BY last_used LIMIT ?2)";
            Stmt s(m_db, sql);
            s.bindText(1, cutoff.toUTC().toString(Qt::ISODateWithMs)).bindInt(2, limit - deleted);
            s.run();
            const qint64 n = m_db.changes();
            deleted += n;
            if (deleted >= limit) {
                limited = true; // the limit was reached: there may be more
            }
        }
        if (!ids.empty()) {
            Stmt s(m_db, "DELETE FROM clips WHERE id = ?1 AND is_saved = 0 AND secret = 0");
            for (const qint64 id : ids) {
                if (deleted >= limit) {
                    limited = true;
                    break;
                }
                s.reset();
                s.bindInt(1, id);
                s.run();
                deleted += m_db.changes();
            }
        }
        tx.commit();
    }
    if (more) {
        *more = limited;
    }
    if (deleted > 0) {
        m_scrubOwed = true;
    }
    if (m_scrubOwed && !limited) {
        m_scrubOwed = false;
        scrub(); // freed pages were zeroed (secure_delete); the index and the log may still hold the text
    }
    return deleted;
}

qint64 Repository::pruneImages(int maxUnsaved)
{
    Stmt s(m_db, R"(
        DELETE FROM clips WHERE id IN (
            SELECT id FROM clips WHERE is_saved = 0 AND kind = 'image'
            ORDER BY last_used ASC, id ASC
            LIMIT max(0, (SELECT count(*) FROM clips WHERE is_saved = 0 AND kind = 'image') - ?1)))");
    s.bindInt(1, qMax(0, maxUnsaved));
    s.run();
    return m_db.changes();
}

qint64 Repository::countImages()
{
    Stmt s(m_db, "SELECT count(*) FROM clips WHERE kind = 'image'");
    s.step();
    return s.colInt(0);
}

QStringList Repository::groups()
{
    QStringList result;
    for (const GroupInfo& g : groupTree()) {
        result << g.path;
    }
    return result;
}

bool Repository::moveGroup(const QString& path, int delta)
{
    const QString normalized = groups::normalize(path);
    const QString parent = groups::parent(normalized);
    QStringList siblings; // in their current order
    for (const GroupInfo& g : groupTree()) {
        if (groups::parent(g.path) == parent) {
            siblings << g.path;
        }
    }
    const qsizetype from = siblings.indexOf(normalized);
    const qsizetype to = from + delta;
    if (from < 0 || delta == 0 || to < 0 || to >= siblings.size()) {
        return false;
    }
    siblings.swapItemsAt(from, to);
    // Number every sibling, so the order no longer depends on names.
    Transaction tx(m_db);
    Stmt s(m_db, "UPDATE groups SET position = ?2 WHERE path = ?1");
    for (qsizetype i = 0; i < siblings.size(); ++i) {
        s.reset();
        s.bindText(1, siblings[i]).bindInt(2, i);
        s.run();
    }
    tx.commit();
    return true;
}

qint64 Repository::count()
{
    m_count->reset();
    m_count->step();
    const qint64 n = m_count->colInt(0);
    m_count->reset();
    return n;
}

qint64 Repository::countUnsaved()
{
    m_countUnsaved->reset();
    m_countUnsaved->step();
    const qint64 n = m_countUnsaved->colInt(0);
    m_countUnsaved->reset();
    return n;
}

// --- Sync

namespace {

void setMeta(Db& db, const char* key, const QString& value)
{
    Stmt s(db, "INSERT INTO meta(key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bindText(1, QString::fromLatin1(key)).bindText(2, value).run();
}

QString isoStamp(const QDateTime& time)
{
    return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString();
}

} // namespace

void Repository::setSyncTracking(bool on)
{
    setMeta(m_db, "sync_on", on ? QStringLiteral("1") : QStringLiteral("0"));
    if (!on) {
        m_db.exec("DELETE FROM sync_tombstones"); // nothing of what was deleted is kept when sync is off
    }
}

bool Repository::syncTracking()
{
    Stmt s(m_db, "SELECT value FROM meta WHERE key = 'sync_on'");
    return s.step() && s.colText(0) == QLatin1String("1");
}

QString Repository::syncDeviceId()
{
    {
        Stmt s(m_db, "SELECT value FROM meta WHERE key = 'sync_device_id'");
        if (s.step() && !s.colText(0).isEmpty()) {
            return s.colText(0);
        }
    }
    const QString id = QString::fromLatin1(PasswordVault::randomBytes(16).toHex());
    setMeta(m_db, "sync_device_id", id);
    return id;
}

std::vector<Repository::SyncCopy> Repository::syncExportList(int maxUnsaved, bool pictures, qint64 maxPictureBytes)
{
    const char* eligible = "c.secret = 0 AND c.paste_once = 0 AND (c.kind = 'text' OR (?1 = 1 AND "
                           "(SELECT length(png) FROM clip_images i WHERE i.clip_id = c.id) <= ?2))";
    const QByteArray sql =
        QByteArray("SELECT c.id, c.content_hash, c.last_used, coalesce(c.meta_changed, ''), c.kind FROM clips c WHERE ")
        + eligible + " AND (c.is_saved = 1 OR c.id IN (SELECT c.id FROM clips c WHERE c.is_saved = 0 AND " + eligible
        + " ORDER BY c.last_used DESC, c.id DESC LIMIT ?3)) ORDER BY c.id";
    Stmt s(m_db, sql);
    s.bindInt(1, pictures ? 1 : 0).bindInt(2, maxPictureBytes).bindInt(3, maxUnsaved);
    std::vector<SyncCopy> out;
    while (s.step()) {
        out.push_back({s.colInt(0), s.colText(1), s.colText(2), s.colText(3), s.colText(4) == QLatin1String("image")});
    }
    return out;
}

std::optional<Repository::SyncItem> Repository::syncItem(qint64 id)
{
    auto e = get(id);
    if (!e || e->secret || e->pasteOnce) {
        return std::nullopt;
    }
    SyncItem item;
    item.entry = *e;
    Stmt s(m_db, "SELECT coalesce(meta_changed, ''), off_recent FROM clips WHERE id = ?1");
    s.bindInt(1, id);
    if (s.step()) {
        item.metaChanged = s.colText(0);
        item.offRecent = s.colInt(1) != 0;
    }
    if (e->image) {
        item.png = imageData(id);
    }
    return item;
}

QString Repository::syncFingerprint()
{
    Stmt s(m_db, "SELECT value FROM meta WHERE key = 'sync_fingerprint'");
    return s.step() ? s.colText(0) : QString();
}

void Repository::setSyncFingerprint(const QString& fingerprint)
{
    setMeta(m_db, "sync_fingerprint", fingerprint);
}

std::vector<Repository::Tombstone> Repository::syncSharedTombstones(const QString& since)
{
    Stmt s(m_db, "SELECT t.content_hash, t.deleted_at FROM sync_tombstones t WHERE t.shared = 1 AND t.deleted_at >= ?1 "
                 "AND NOT EXISTS (SELECT 1 FROM clips c WHERE c.content_hash = t.content_hash AND c.last_used > t.deleted_at) "
                 "ORDER BY t.content_hash");
    s.bindText(1, since);
    std::vector<Tombstone> out;
    while (s.step()) {
        out.push_back({s.colText(0), s.colText(1)});
    }
    return out;
}

QString Repository::syncTombstone(const QString& hash)
{
    Stmt s(m_db, "SELECT deleted_at FROM sync_tombstones WHERE content_hash = ?1");
    s.bindText(1, hash);
    return s.step() ? s.colText(0) : QString();
}

void Repository::syncPurgeTombstones(const QString& before)
{
    Stmt s(m_db, "DELETE FROM sync_tombstones WHERE deleted_at < ?1");
    s.bindText(1, before).run();
}

void Repository::syncBatch(const std::function<void()>& work)
{
    Transaction tx(m_db);
    setMeta(m_db, "sync_applying", QStringLiteral("1"));
    work();
    setMeta(m_db, "sync_applying", QStringLiteral("0"));
    tx.commit();
}

qint64 Repository::syncAdd(const SyncItem& item, const ImageClip& image)
{
    const Entry& e = item.entry;
    qint64 id = 0;
    if (e.image) {
        Stmt& s = *m_ingestImage;
        s.reset();
        s.bindText(1, image.label()).bindText(2, QStringLiteral("image:") + image.pixelHash).bindText(3, isoStamp(e.lastUsed));
        s.step();
        id = s.colInt(0);
        s.run();
        Stmt store(m_db, "INSERT OR IGNORE INTO clip_images(clip_id, png, thumbnail, width, height) VALUES (?1, ?2, ?3, ?4, ?5)");
        store.bindInt(1, id).bindBlob(2, image.png).bindBlob(3, image.thumbnail);
        store.bindInt(4, image.width).bindInt(5, image.height);
        store.run();
    } else {
        Stmt& s = *m_ingest;
        s.reset();
        s.bindText(1, e.content).bindText(2, textHash(e.content)).bindText(3, isoStamp(e.lastUsed));
        s.step();
        id = s.colInt(0);
        s.run();
    }
    const QString path = groups::normalize(e.group);
    if (!path.isEmpty()) {
        createGroup(path);
    }
    // The columns the search index covers are written only when there is something to write (see restoreFiling): an
    // UPDATE that names them rewrites the row's index entries, which made a large merge slow.
    const QString noteText = e.note.trimmed();
    const bool filed = !noteText.isEmpty() || !path.isEmpty();
    Stmt s(m_db, filed ? "UPDATE clips SET created_at = ?2, last_used = ?3, tag_or_note = ?4, group_name = ?5, is_favorite = ?6, "
                         "off_recent = ?7, meta_changed = ?8 WHERE id = ?1"
                       : "UPDATE clips SET created_at = ?2, last_used = ?3, is_favorite = ?6, off_recent = ?7, meta_changed = ?8 "
                         "WHERE id = ?1");
    s.bindInt(1, id).bindText(2, isoStamp(e.createdAt.isValid() ? e.createdAt : e.lastUsed)).bindText(3, isoStamp(e.lastUsed));
    if (filed) {
        s.bindTextOrNull(4, noteText).bindTextOrNull(5, path);
    }
    s.bindInt(6, e.favorite ? 1 : 0).bindInt(7, item.offRecent ? 1 : 0);
    s.bindTextOrNull(8, item.metaChanged).run();
    return id;
}

void Repository::syncSetMeta(qint64 id, const QString& note, const QString& group, bool favorite, bool pinned,
                             const QString& metaChanged)
{
    const QString path = groups::normalize(group);
    if (!path.isEmpty()) {
        createGroup(path);
    }
    {
        Stmt s(m_db, "UPDATE clips SET tag_or_note = ?2, group_name = ?3, is_favorite = ?4, meta_changed = ?5 WHERE id = ?1");
        s.bindInt(1, id).bindTextOrNull(2, note.trimmed()).bindTextOrNull(3, path).bindInt(4, favorite ? 1 : 0);
        s.bindTextOrNull(5, metaChanged).run();
    }
    const auto e = get(id);
    if (e && e->pinned() != pinned) {
        setPinned(id, pinned); // refused when the pin list is full; the trigger is off, so meta_changed stays
    }
}

bool Repository::syncDelete(qint64 id)
{
    Stmt s(m_db, "DELETE FROM clips WHERE id = ?1");
    s.bindInt(1, id).run();
    return m_db.changes() > 0;
}

void Repository::syncScrub()
{
    scrub();
}

std::pair<QString, qint64> Repository::syncOldestUnsaved(bool images)
{
    Stmt s(m_db, "SELECT min(last_used), count(*) FROM clips WHERE is_saved = 0 AND secret = 0 AND kind = ?1");
    s.bindText(1, images ? QStringLiteral("image") : QStringLiteral("text"));
    s.step();
    return {s.colText(0), s.colInt(1)};
}

void Repository::optimize()
{
    m_db.exec("INSERT INTO clips_fts(clips_fts) VALUES ('optimize'); PRAGMA optimize;");
}

} // namespace clipitto::db

namespace clipitto::db {

// --- Auto grouping (AutoGroup.h) ----------------------------------------------------------------

void Repository::setAutoGrouping(bool on)
{
    m_autoGrouping = on;
}

void Repository::learn(const QString& group, const QString& text, int delta)
{
    const auto toks = autogroup::tokens(text);
    if (toks.empty() || group.isEmpty()) {
        return;
    }
    Nested tx(m_db);
    if (delta > 0) {
        Stmt up(m_db, "INSERT INTO auto_tokens(group_name, token, weight) VALUES (?1, ?2, ?3) "
                      "ON CONFLICT(group_name, token) DO UPDATE SET weight = weight + excluded.weight");
        for (const autogroup::Token& t : toks) {
            up.reset();
            up.bindText(1, group).bindInt(2, t.hash).bindInt(3, delta).run();
        }
    } else {
        Stmt down(m_db, "UPDATE auto_tokens SET weight = weight + ?3 WHERE group_name = ?1 AND token = ?2");
        for (const autogroup::Token& t : toks) {
            down.reset();
            down.bindText(1, group).bindInt(2, t.hash).bindInt(3, delta).run();
        }
        Stmt clean(m_db, "DELETE FROM auto_tokens WHERE group_name = ?1 AND weight <= 0");
        clean.bindText(1, group).run();
    }
    Stmt count(m_db, "INSERT INTO auto_examples(group_name, examples) VALUES (?1, MAX(0, ?2)) "
                     "ON CONFLICT(group_name) DO UPDATE SET examples = MAX(0, examples + ?2)");
    count.bindText(1, group).bindInt(2, delta > 0 ? 1 : -1).run();
    tx.commit();
    m_autoModel.reset();
}

void Repository::ensureAutoSeeded()
{
    {
        Stmt seeded(m_db, "SELECT 1 FROM meta WHERE key = 'auto_seeded'");
        if (seeded.step()) {
            return;
        }
    }
    // Copies already in groups are the first examples (those filed automatically are not: they are guesses).
    std::vector<std::pair<QString, QString>> examples;
    {
        Stmt s(m_db, "SELECT group_name, content FROM clips WHERE kind = 'text' AND secret = 0 AND auto_filed = 0 "
                     "AND group_name IS NOT NULL AND group_name <> ''");
        while (s.step()) {
            const QString content = s.colText(1);
            if (!looksSecret(content)) {
                examples.emplace_back(s.colText(0), content);
            }
        }
    }
    Nested tx(m_db);
    for (const auto& [group, text] : examples) {
        learn(group, text, 1);
    }
    Stmt mark(m_db, "INSERT INTO meta(key, value) VALUES ('auto_seeded', '1') "
                    "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    mark.run();
    tx.commit();
}

std::shared_ptr<const autogroup::Model> Repository::autoModel()
{
    if (m_autoModel) {
        return m_autoModel;
    }
    ensureAutoSeeded();
    auto model = std::make_shared<autogroup::Model>();
    Stmt s(m_db, "SELECT group_name, token, weight FROM auto_tokens");
    while (s.step()) {
        model->add(s.colText(0), s.colInt(1), int(s.colInt(2)));
    }
    m_autoModel = model;
    return m_autoModel;
}

bool Repository::setAutoGroup(qint64 id, const QString& group)
{
    const QString target = groups::normalize(group);
    if (target.isEmpty()) {
        return false;
    }
    {
        Stmt exists(m_db, "SELECT 1 FROM groups WHERE path = ?1");
        exists.bindText(1, target);
        if (!exists.step()) {
            return false; // never creates a group
        }
    }
    // Only a plain, still unfiled copy that is not saved in any way: the user may have acted since.
    Stmt s(m_db, "UPDATE clips SET group_name = ?2, auto_filed = 1 WHERE id = ?1 AND kind = 'text' AND secret = 0 "
                 "AND is_saved = 0 AND (group_name IS NULL OR group_name = '') AND auto_filed = 0");
    s.bindInt(1, id).bindText(2, target).run();
    return m_db.changes() > 0;
}

std::vector<Repository::AutoExamples> Repository::autoExamples()
{
    std::vector<AutoExamples> out;
    Stmt s(m_db, "SELECT group_name, examples FROM auto_examples WHERE examples > 0 ORDER BY group_name");
    while (s.step()) {
        out.push_back({s.colText(0), int(s.colInt(1))});
    }
    return out;
}

void Repository::forgetAutoLearned()
{
    Nested tx(m_db);
    m_db.exec("DELETE FROM auto_tokens");
    m_db.exec("DELETE FROM auto_examples");
    // Forgotten means forgotten: the groups' copies are not read again as first examples.
    m_db.exec("INSERT INTO meta(key, value) VALUES ('auto_seeded', '1') ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    tx.commit();
    m_autoModel.reset();
    ++m_autoForgets;
    checkpoint(); // the log may still hold the forgotten rows
}

} // namespace clipitto::db
