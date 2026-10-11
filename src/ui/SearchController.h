#pragma once

#include <QObject>
#include <QTimer>

namespace clipitto {
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
};

} // namespace clipitto::ui
