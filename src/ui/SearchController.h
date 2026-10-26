#pragma once

#include <QObject>
#include <QTimer>

namespace clipitto {
class PasswordKeeper;
struct Config;
namespace db {
class Repository;
}
} // namespace clipitto

namespace clipitto::ui {

class HistoryModel;

// Turns the search text into model rows. Non-empty queries are debounced; clearing the
// query switches back to the default view (group tree + saved items + recent copies)
// immediately. Search results are a flat list.
class SearchController : public QObject {
    Q_OBJECT

public:
    SearchController(db::Repository& repo, HistoryModel& model, QObject* parent = nullptr);

    void applyConfig(const Config& config);

    void setQuery(const QString& query);
    QString query() const { return m_query; }

    // Password copies (may be null: none). A password search lists only them: those
    // matching `query` (part of a password when unlocked, the whole one when locked), or
    // all of them while it is empty. Inactive: back to the normal query.
    void setPasswords(const PasswordKeeper* keeper) { m_passwords = keeper; }
    void setPasswordQuery(const QString& query, bool active);
    bool isPasswordSearch() const { return m_passwordSearch; }
    // The last password search found nothing while locked: an unlock may find more.
    bool passwordSearchNeedsUnlock() const { return m_needsUnlock; }
    void refresh(); // re-run the current query now
    bool isPending() const { return m_debounce.isActive(); }
    // Suspended: no queries at all (pending ones are dropped) until resumed.
    void setSuspended(bool suspended);

    int queriesRun() const { return m_queriesRun; } // for tests and diagnostics

    static constexpr int searchLimit = 100;

signals:
    void updated(); // the model was repopulated

private:
    void run();

    db::Repository& m_repo;
    HistoryModel& m_model;
    QTimer m_debounce;
    QString m_query;
    int m_recentCount = 20;
    int m_savedCap = 15;
    bool m_showSavedOnTop = true;
    int m_queriesRun = 0;
    bool m_suspended = false;
    const PasswordKeeper* m_passwords = nullptr;
    bool m_passwordSearch = false;
    QString m_passwordQuery;
    bool m_needsUnlock = false;
};

} // namespace clipitto::ui
