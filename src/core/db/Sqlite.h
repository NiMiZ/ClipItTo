#pragma once

#include <QString>
#include <QtGlobal>

#include <stdexcept>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace clipitto::db {

class DbError : public std::runtime_error {
public:
    DbError(const std::string& what, int code) : std::runtime_error(what), m_code(code) {}
    int code() const { return m_code; }

private:
    int m_code;
};

// True when built with SQLite3 Multiple Ciphers (CLIPITTO_ENCRYPTION).
bool encryptionSupported();

enum class FileState { Missing, Plain, Encrypted };
// Whether `path` is absent, a plain database, or an encrypted one (no SQLite header and
// unreadable without a key). A damaged plain database throws instead of passing as encrypted.
FileState probeFile(const QString& path);

// Overwrites a buffer that held key material before it is released.
void wipe(QByteArray& secret);

// Owns a sqlite3 connection. Not thread-safe: use from one thread.
class Db {
public:
    // ":memory:" for an in-memory database. A non-empty `key` (raw bytes) opens an encrypted
    // database; a wrong key throws.
    explicit Db(const QString& path, const QByteArray& key = {});
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    sqlite3* handle() const { return m_db; }
    void exec(const char* sql);
    void exec(const QString& sql);
    qint64 changes() const;

    // Encrypts a plain database in place with `key`, re-keys an encrypted one, or (empty
    // key) decrypts it. Journaled, so an interruption leaves the old state. Prepared
    // statements stay valid; none may be mid-step.
    void rekey(const QByteArray& key);

private:
    sqlite3* m_db = nullptr;
};

// Applies the connection PRAGMAs (WAL, synchronous=NORMAL, busy timeout, ...).
void configureConnection(Db& db);

// A prepared statement meant to be prepared once and reused.
// Call reset() before binding for a new execution.
class Stmt {
public:
    Stmt(Db& db, const char* sql);
    Stmt(Db& db, const QByteArray& sql) : Stmt(db, sql.constData()) {}
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bindText(int index, const QString& value); // 1-based, as in SQLite
    Stmt& bindTextOrNull(int index, const QString& value); // empty -> NULL
    Stmt& bindInt(int index, qint64 value);
    Stmt& bindNull(int index);

    bool step(); // true while a row is available
    void run(); // step to completion, then reset
    void reset(); // reset and clear bindings

    qint64 colInt(int col) const; // 0-based, as in SQLite
    QString colText(int col) const; // NULL -> null QString
    bool colIsNull(int col) const;

private:
    [[noreturn]] void fail(int rc, const char* what) const;

    sqlite3_stmt* m_stmt = nullptr;
};

// BEGIN IMMEDIATE on construction; rolls back unless commit() was called.
class Transaction {
public:
    explicit Transaction(Db& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();

private:
    Db& m_db;
    bool m_done = false;
};

} // namespace clipitto::db
