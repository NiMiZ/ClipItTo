#include "db/Repository.h"

#include "GroupPath.h"
#include "db/FtsQuery.h"
#include "db/Sqlite.h"

#include <QCryptographicHash>

namespace clipitto::db {

namespace {

// Column order read by readEntry().
#define CLIP_COLUMNS "c.id, c.content, c.tag_or_note, c.group_name, c.is_favorite, c.created_at, c.last_used"

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
    return e;
}

} // namespace

Repository::Repository(Db& db, Clock clock)
    : m_db(db)
    , m_clock(clock ? std::move(clock) : Clock([] { return QDateTime::currentDateTimeUtc(); }))
{
    // created_at = last_used only holds right after an insert, which tells us whether the upsert inserted.
    m_ingest = std::make_unique<Stmt>(m_db, R"(
        INSERT INTO clips(content, content_hash, created_at, last_used) VALUES (?1, ?2, ?3, ?3)
        ON CONFLICT(content_hash) DO UPDATE SET last_used = excluded.last_used
        RETURNING id, created_at = last_used)");

    m_recentAll = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS R"( FROM clips c
        ORDER BY c.last_used DESC, c.id DESC LIMIT ?1)");

    m_recentUnsaved = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS R"( FROM clips c
        WHERE c.is_saved = 0
        ORDER BY c.last_used DESC, c.id DESC LIMIT ?1)");

    m_saved = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS R"( FROM clips c
        WHERE c.is_saved = 1
        ORDER BY c.is_favorite DESC, c.last_used DESC, c.id DESC LIMIT ?1)");

    // bm25 column weights: note 10, group 5, content 1 (lower score = better match).
    m_searchFts = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS R"(,
            bm25(clips_fts, 10.0, 5.0, 1.0) AS score
        FROM clips_fts JOIN clips c ON c.id = clips_fts.rowid
        WHERE clips_fts MATCH ?1
        ORDER BY c.is_saved DESC, score ASC, c.last_used DESC, c.id DESC
        LIMIT ?2)");

    m_get = std::make_unique<Stmt>(m_db, "SELECT " CLIP_COLUMNS " FROM clips c WHERE c.id = ?1");
    m_touch = std::make_unique<Stmt>(m_db, "UPDATE clips SET last_used = ?2 WHERE id = ?1");
    m_setFavorite = std::make_unique<Stmt>(m_db, "UPDATE clips SET is_favorite = ?2 WHERE id = ?1");
    m_setGroup = std::make_unique<Stmt>(m_db, "UPDATE clips SET group_name = ?2 WHERE id = ?1");
    m_setNote = std::make_unique<Stmt>(m_db, "UPDATE clips SET tag_or_note = ?2 WHERE id = ?1");
    m_remove = std::make_unique<Stmt>(m_db, "DELETE FROM clips WHERE id = ?1");

    m_prune = std::make_unique<Stmt>(m_db, R"(
        DELETE FROM clips WHERE id IN (
            SELECT id FROM clips WHERE is_saved = 0
            ORDER BY last_used ASC, id ASC
            LIMIT max(0, (SELECT count(*) FROM clips WHERE is_saved = 0) - ?1)))");

    m_groups = std::make_unique<Stmt>(m_db, "SELECT path FROM groups ORDER BY path COLLATE NOCASE");

    m_count = std::make_unique<Stmt>(m_db, "SELECT count(*) FROM clips");
    m_countUnsaved = std::make_unique<Stmt>(m_db, "SELECT count(*) FROM clips WHERE is_saved = 0");
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

Repository::IngestResult Repository::ingest(const QString& text)
{
    Stmt& s = *m_ingest;
    s.reset();
    s.bindText(1, text).bindText(2, hashOf(text)).bindText(3, now());
    s.step();
    const IngestResult result{s.colInt(0), s.colInt(1) != 0};
    s.run();
    return result;
}

std::vector<Entry> Repository::collect(Stmt& stmt)
{
    std::vector<Entry> result;
    while (stmt.step()) {
        result.push_back(readEntry(stmt));
    }
    stmt.reset();
    return result;
}

std::vector<Entry> Repository::recent(int limit, bool unsavedOnly)
{
    Stmt& s = unsavedOnly ? *m_recentUnsaved : *m_recentAll;
    s.reset();
    s.bindInt(1, limit);
    return collect(s);
}

std::vector<Entry> Repository::saved(int limit)
{
    m_saved->reset();
    m_saved->bindInt(1, limit);
    return collect(*m_saved);
}

std::vector<Entry> Repository::search(const QString& query, int limit, SearchMode mode)
{
    const QStringList tokens = fts::tokenize(query);
    if (tokens.isEmpty()) {
        return {};
    }
    if (mode == SearchMode::ForceLike || !fts::canUseTrigram(tokens)) {
        return searchLike(tokens, limit);
    }
    Stmt& s = *m_searchFts;
    s.reset();
    s.bindText(1, fts::matchExpression(tokens)).bindInt(2, limit);
    return collect(s);
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
                                       "ORDER BY c.is_saved DESC, note_hits DESC, group_hits DESC, "
                                       "c.last_used DESC, c.id DESC LIMIT ?%4")
                            .arg(noteHits.join(QLatin1String(" + ")), groupHits.join(QLatin1String(" + ")),
                                 where.join(QLatin1String(" AND ")))
                            .arg(tokenCount + 1);

    auto stmt = std::make_unique<Stmt>(m_db, sql.toUtf8());
    Stmt& ref = *stmt;
    m_likeByTokenCount.emplace(tokenCount, std::move(stmt));
    return ref;
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

void Repository::setFavorite(qint64 id, bool favorite)
{
    m_setFavorite->reset();
    m_setFavorite->bindInt(1, id).bindInt(2, favorite ? 1 : 0);
    m_setFavorite->run();
}

void Repository::setGroup(qint64 id, const QString& group)
{
    const QString path = group.trimmed().isEmpty() ? QString() : createGroup(group);
    m_setGroup->reset();
    m_setGroup->bindInt(1, id).bindTextOrNull(2, path);
    m_setGroup->run();
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
    Stmt s(m_db, R"(
        SELECT g.path, (SELECT count(*) FROM clips c WHERE c.group_name = g.path)
        FROM groups g ORDER BY g.path COLLATE NOCASE)");
    std::vector<GroupInfo> result;
    while (s.step()) {
        result.push_back({s.colText(0), int(s.colInt(1))});
    }
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
         }) {
        Stmt s(m_db, sql);
        s.bindText(1, source).bindText(2, target).bindText(3, descendantsPattern(source));
        s.run();
    }
    tx.commit();
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
             "UPDATE clips SET group_name = NULL WHERE group_name = ?1 OR group_name LIKE ?2 ESCAPE '\\'",
             "DELETE FROM groups WHERE path = ?1 OR path LIKE ?2 ESCAPE '\\'",
         }) {
        Stmt s(m_db, sql);
        s.bindText(1, normalized).bindText(2, descendantsPattern(normalized));
        s.run();
    }
    tx.commit();
}

void Repository::setNote(qint64 id, const QString& note)
{
    m_setNote->reset();
    m_setNote->bindInt(1, id).bindTextOrNull(2, note.trimmed());
    m_setNote->run();
}

void Repository::remove(qint64 id)
{
    m_remove->reset();
    m_remove->bindInt(1, id);
    m_remove->run();
}

qint64 Repository::prune(int maxUnsaved)
{
    m_prune->reset();
    m_prune->bindInt(1, maxUnsaved);
    m_prune->run();
    return m_db.changes();
}

QStringList Repository::groups()
{
    QStringList result;
    m_groups->reset();
    while (m_groups->step()) {
        result << m_groups->colText(0);
    }
    m_groups->reset();
    return result;
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

void Repository::optimize()
{
    m_db.exec("INSERT INTO clips_fts(clips_fts) VALUES ('optimize'); PRAGMA optimize;");
}

} // namespace clipitto::db
