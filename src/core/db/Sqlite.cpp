#include "db/Sqlite.h"

#include <sqlite3.h>

namespace clipitto::db {

namespace {

std::string errorMessage(sqlite3* db, int rc)
{
    return db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
}

} // namespace

Db::Db(const QString& path)
{
    const QByteArray utf8 = path.toUtf8();
    const int rc = sqlite3_open_v2(utf8.constData(), &m_db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        const std::string msg = errorMessage(m_db, rc);
        sqlite3_close(m_db);
        m_db = nullptr;
        throw DbError("cannot open database " + utf8.toStdString() + ": " + msg, rc);
    }
    sqlite3_extended_result_codes(m_db, 1);
}

Db::~Db()
{
    sqlite3_close_v2(m_db);
}

void Db::exec(const char* sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const std::string msg = err ? err : sqlite3_errstr(rc);
        sqlite3_free(err);
        throw DbError(msg, rc);
    }
}

void Db::exec(const QString& sql)
{
    exec(sql.toUtf8().constData());
}

qint64 Db::changes() const
{
    return sqlite3_changes64(m_db);
}

void configureConnection(Db& db)
{
    sqlite3_busy_timeout(db.handle(), 2000);
    db.exec("PRAGMA journal_mode = WAL;"
            "PRAGMA synchronous = NORMAL;"
            "PRAGMA foreign_keys = ON;"
            "PRAGMA temp_store = MEMORY;");
}

Stmt::Stmt(Db& db, const char* sql)
{
    const int rc = sqlite3_prepare_v3(db.handle(), sql, -1, SQLITE_PREPARE_PERSISTENT, &m_stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw DbError("prepare failed: " + errorMessage(db.handle(), rc) + "\n  in: " + sql, rc);
    }
}

Stmt::~Stmt()
{
    sqlite3_finalize(m_stmt);
}

Stmt& Stmt::bindText(int index, const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    const int rc = sqlite3_bind_text64(m_stmt, index, utf8.constData(), sqlite3_uint64(utf8.size()),
                                       SQLITE_TRANSIENT, SQLITE_UTF8);
    if (rc != SQLITE_OK) {
        fail(rc, "bind");
    }
    return *this;
}

Stmt& Stmt::bindTextOrNull(int index, const QString& value)
{
    return value.isEmpty() ? bindNull(index) : bindText(index, value);
}

Stmt& Stmt::bindInt(int index, qint64 value)
{
    const int rc = sqlite3_bind_int64(m_stmt, index, value);
    if (rc != SQLITE_OK) {
        fail(rc, "bind");
    }
    return *this;
}

Stmt& Stmt::bindNull(int index)
{
    const int rc = sqlite3_bind_null(m_stmt, index);
    if (rc != SQLITE_OK) {
        fail(rc, "bind");
    }
    return *this;
}

bool Stmt::step()
{
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    fail(rc, "step");
}

void Stmt::run()
{
    while (step()) {
    }
    sqlite3_reset(m_stmt);
}

void Stmt::reset()
{
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
}

qint64 Stmt::colInt(int col) const
{
    return sqlite3_column_int64(m_stmt, col);
}

QString Stmt::colText(int col) const
{
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt, col));
    if (!text) {
        return {};
    }
    return QString::fromUtf8(text, sqlite3_column_bytes(m_stmt, col));
}

bool Stmt::colIsNull(int col) const
{
    return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
}

void Stmt::fail(int rc, const char* what) const
{
    sqlite3* db = sqlite3_db_handle(m_stmt);
    const std::string sql = sqlite3_sql(m_stmt) ? sqlite3_sql(m_stmt) : "";
    sqlite3_reset(m_stmt); // leave the statement reusable
    throw DbError(std::string(what) + " failed: " + errorMessage(db, rc) + "\n  in: " + sql, rc);
}

Transaction::Transaction(Db& db) : m_db(db)
{
    m_db.exec("BEGIN IMMEDIATE");
}

Transaction::~Transaction()
{
    if (!m_done) {
        sqlite3_exec(m_db.handle(), "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

void Transaction::commit()
{
    m_db.exec("COMMIT");
    m_done = true;
}

} // namespace clipitto::db
